// Hardware-encoded preview (h264_rkmpp) in MPEG-TS.
// Default transport is TCP pull: the board listens on net_port,
// the viewer connects when it wants (ffplay tcp://<board-ip>:1234).
// The legacy UDP push to net_host (--net-host) remains available:
// UDP has no "connect", the viewer joins whenever it wants,
// and a lost packet glitches briefly until the next IDR
// instead of stalling the whole flow like TCP over Wi-Fi.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// DRM frame descriptor for the encoder zero-copy input: 
// the ping-pong dma-bufs are wrapped as AV_PIX_FMT_DRM_PRIME frames and 
// imported by the rkmpp wrapper by fd -- no CPU pixel copies anywhere in the path.
#include <libavutil/hwcontext_drm.h>

#include "pod_ring.hpp"

// The DRM_FORMAT_* constants (NV12, MOD_LINEAR) live in drm_fourcc.h:
// this header uses only the descriptor TYPE (hwcontext_drm.h) --
// the constants are the cpp's business.

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>

#include "rga_resizer.h"       // LbGeom: band geometry arrives as one object
#include "rkmpp_drm_pool.hpp"  // RkmppDrmPool<kBufCount>: RAII pool sub-object
#include "tcp_pull_server.hpp" // TcpPullServer: RAII single-viewer TCP pull transport

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct AVIOContext;

class NetEncoder
{
    // One viewer session's transport (TCP pull or UDP push), RAII:
    // the destructor is the single teardown point, the early-return
    // paths of run_session() free everything by scope exit.
    struct Transport
    {
        ~Transport();
        void close();

    public:
        TcpPullServer* tcp = nullptr;
        AVIOContext* udp_pb = nullptr;
        bool tcp_pull = false;
    };

    // RAII guard for the muxer context (run_session): p->pb is nulled first
    // -- the Transport destructor owns the avio context and closes it 
    // -- then the context is freed. Same declaration position as 
    // Transport -> same destruction order: fc dies AFTER the Transport, as before.
    struct FcGuard
    {
        ~FcGuard();
        FcGuard( const FcGuard& ) = delete;
        FcGuard& operator=( const FcGuard& ) = delete;

    public:
        AVFormatContext* p;
    };

    enum
    {
        kBufCount = 4,
        kPoolCount = kBufCount + 1,
        kPktQDepth = 32
    };

public:
    NetEncoder( int model_w,
                int model_h,
                const LbGeom& band,
                int fps,
                int port,
                const std::string& host,
                int bitrate_kbps,
                int nsrc = 1 );

    ~NetEncoder();
    NetEncoder( const NetEncoder& ) = delete;
    NetEncoder& operator=( const NetEncoder& ) = delete;

    // Called once per frame from the pipeline loop. tensor_fd is the
    // dma-buf of the NPU input tensor (RGB888, w x h); boxes are drawn.
    // ts_us is the dequeue timestamp in microseconds from pipeline
    // start -- the pts source in follow-source mode (--net-fps 0, VFR);
    // ignored in the fixed-fps mode.
    void publish( int tensor_fd, const LbGeom& band, int64_t ts_us = -1, int src_idx = 0 );

    // NO SIGNAL publish (idle): RGA-copies the pre-rendered idle canvas
    // (gray + the plate) into the ring slot / composite canvas cells of
    // every absent source, then encodes one frame. Called by
    // Pipeline::idle_tick while at least one configured tile has no live
    // source; idle encodes share the last_enc_ts_us_ budget with live
    // ones and are NOT counted in --frames or the timing stats.
    void publish_idle( int64_t ts_us, const bool* absent, int nabs );

    // True while the stream path is live (viewer connected in PULL
    // mode / UDP path open). The pipeline gates GPU box drawing on it:
    // recognition runs continuously, output does not.
    bool viewer_active() const;

private:
    void serve();
    bool send_frame( AVStream* st );
    // The mux side split out of serve() for readability:
    // drain_queue() owns the wait/pop loop, mux_write() writes one
    // packet with the IDR-prepend logic; hdr_/hdr_logged_ stay members.
    void drain_queue( AVFormatContext* fc );
    // One finished encoder packet -> the preallocated ring queue for
    // serve() to mux; shared by the EAGAIN retry and the send drain.
    void enqueue_pkt( AVPacket* pkt, AVStream* st, int64_t fpts );
    void run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url );

    // Transport setup (accept + socket options / avio_open) and the
    // stream setup (new_stream + header + mpegts options).
    bool open_transport( const std::string& url, AVFormatContext* fc, Transport& t );
    bool setup_stream( AVFormatContext* fc, AVStream** st, bool tcp_pull );

    bool init_frames();
    void free_frames();
    void clear_pkt_queue();
    int64_t packet_pts();  // pts of the next frame; may set ts_base_
    int mux_write( AVFormatContext* fc, AVPacket* pkt );
    bool prep_frame( int tensor_fd, const LbGeom& band, int src_idx, MppBuffer mb );

    // NO SIGNAL frame assembly (idle path): copies the idle-plate tile(s)
    // into the target buffer. False on RGA failure.
    bool prep_idle_frame( const bool* absent, int nabs, MppBuffer mb );

private:
    const int w_, h_, tile_w_, tile_h_, nsrc_, fps_, port_, bitrate_kbps_;
    const std::string host_;
    std::thread thread_;
    std::atomic<bool> quit_ { false };
    std::atomic<bool> error_ { false };

    std::mutex mx_;
    std::condition_variable cv_;
    AVFormatContext* fc_ = nullptr;  // mpegts: TCP pull or UDP push
    AVStream* st_ = nullptr;
    // Set after a viewer accepts; the next encode becomes an IDR
    // (the rkmpp wrapper ignores frame->key_frame, so we go via MPP).
    std::atomic< bool > force_idr_ { false };

    AVCodecContext* enc_ = nullptr;  // h264_rkmpp
    AVPacket* pkt_ = nullptr;
    AVPacket* mux_pkt_ = nullptr;
    AVPacket* dup_pkt_ = nullptr;
    // Four buffers. The 3-deep rkmpp task queue can hold a full VFR
    // clump: the 4th frame's RGA write would hit the slot of the oldest
    // in-flight encode. The 4th ring slot absorbs that window.
    RkmppDrmPool<kPoolCount> pool_;  // 4 ring NV12 dma-bufs + 1 composite canvas (multi-view)
    RkmppDrmPool<1> idle_pool_;  // pre-rendered NO SIGNAL canvas: gray + plate, stamped once at init
    uint32_t cur_ = 0;
    AVDRMFrameDescriptor desc_[kBufCount] {};  // per-buffer DRM descriptors (DRM_PRIME)
    std::array<AVFrame*, kBufCount> frames_ {};  // pre-bound DRM_PRIME frames (one per pool slot)
    std::array<LbGeom, kPoolCount> buf_bands_ {}; // last band: NV12 slot (single-view) or canvas cell (multi-view)

    // Encoder -> serve() preallocated packet ring queue. A separate mutex
    // from mx_: network-side wakeups never touch the encode critical section.
    std::mutex pkt_mx_;
    std::condition_variable pkt_cv_;
    PodRing<AVPacket*, kPktQDepth> pkts_ {};
    int pkt_drops_ = 0;
    bool wait_key_ = true;  // pkt_mx_: drain drops packets until the next IDR (see net_encoder.cpp)
    std::atomic<bool> active_ { false };  // stream path live (serve sets/clears under mx_)
    // Draw -> RGA ordering note: sync_cpu() in OverlaySink::draw_and_publish
    // runs before publish() on the same pipeline thread -- no fence fd
    // crosses this boundary (retired, see docs/state.md).
    int64_t frame_step_ = 1;   // pts increment in the muxer's time base
    uint64_t pts_ = 0;
    // Follow-source (--net-fps 0): pts stamped from the real dequeue
    // times. ts_us_ arrives with publish(); ts_base_ is the session
    // start (pts restart from zero when a new viewer connects).
    // frames_sent_ gates the destructor drain in both modes
    // (pts_ alone would be 0 in VFR and skip the flush).
    bool vfr_ = false;
    int64_t ts_us_ = -1;
    int64_t ts_base_ = -1;
    int64_t last_pts_ = -1;
    // Atomic: written under mx_ by the serve thread on session (re)start,
    // read/written by publish() on the pipeline thread WITHOUT mx_ (only the
    // active_ gate in between) -- a plain int64_t was a data race (UB).
    std::atomic<int64_t> last_enc_ts_us_ { -1 };
    uint64_t frames_sent_ = 0;
    int model_w_ = 0, model_h_ = 0;  // NPU tensor geometry (RGB888)
    int band_left_ = 0, band_top_ = 0;  // visible letterbox band in the tensor
    int band_w_ = 0, band_h_ = 0;       // band size (portrait: vertical strip)
    int vstride_ = 0;  // NV12 ver stride (16-aligned h), set in the ctor

    // h264 parameter-set prefix captured from the first key packet,
    // prepended to every following IDR (late-joiner support).
    // Plain members: function-local statics are forbidden outside tools/.
    std::vector< uint8_t > hdr_;
    std::vector< uint8_t > idr_buf_;
    bool hdr_logged_ = false;
};
