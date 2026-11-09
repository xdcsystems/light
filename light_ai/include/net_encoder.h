// Hardware-encoded preview (h264_rkmpp) in MPEG-TS.
// Default transport is TCP pull: the board listens on net_port,
// the viewer connects when it wants (ffplay tcp://<board-ip>:1234).
// The legacy UDP push to net_host (--net-host) remains available:
// UDP has no "connect", the viewer joins whenever it wants,
// and a lost packet glitches briefly until the next IDR
// instead of stalling the whole flow like TCP over Wi-Fi.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// DRM frame descriptor for the encoder zero-copy input: 
// the ping-pong dma-bufs are wrapped as AV_PIX_FMT_DRM_PRIME frames and 
// imported by the rkmpp wrapper by fd -- no CPU pixel copies anywhere in the path.
#include <libavutil/hwcontext_drm.h>

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

// Opaque ffmpeg types, forward-declared: the header compiles without
// the libav* include paths, and the compiler checks the handle types
// at call sites instead of trusting comments.
typedef struct AVFormatContext AVFormatContext;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;
typedef struct AVStream AVStream;
typedef struct AVIOContext AVIOContext;

// Owned packet handle: the mux queue holds AVPackets freed via
// av_packet_free. The deleter is a plain function pointer so the
// header keeps compiling without the libav* include paths; the
// deleter body and its value live in net_encoder.cpp. The one-arg
// unique_ptr ctor is deleted for pointer deleters, so a PktPtr
// cannot be built without the deleter -- forgetting it is a
// compile error, not a leak.
using PktPtr = std::unique_ptr<AVPacket, void ( * )( AVPacket* )>;

class NetEncoder
{
    // One viewer session's transport (TCP pull or UDP push), RAII:
    // the destructor is the single teardown point, the early-return
    // paths of run_session() free everything by scope exit.
    struct Transport
    {
        TcpPullServer* tcp = nullptr;
        AVIOContext* udp_pb = nullptr;
        bool tcp_pull = false;

        ~Transport();
        void close();
    };

    // RAII guard for the muxer context (run_session): p->pb is nulled
    // first -- the Transport destructor owns the avio context and
    // closes it -- then the context is freed. Same declaration
    // position as Transport -> same destruction order: fc dies AFTER
    // the Transport, as before.
    struct FcGuard
    {
        AVFormatContext* p;
        ~FcGuard();
        FcGuard( const FcGuard& ) = delete;
        FcGuard& operator=( const FcGuard& ) = delete;
    };

public:
    NetEncoder( int model_w,
                int model_h,
                const LbGeom& band,
                int fps,
                int port,
                const std::string& host,
                int bitrate_kbps );

    ~NetEncoder();
    NetEncoder( const NetEncoder& ) = delete;
    NetEncoder& operator=( const NetEncoder& ) = delete;

    // Called once per frame from the pipeline loop. tensor_fd is the
    // dma-buf of the NPU input tensor (RGB888, w x h); boxes are drawn.
    // ts_us is the dequeue timestamp in microseconds from pipeline
    // start -- the pts source in follow-source mode (--net-fps 0, VFR);
    // ignored in the fixed-fps mode.
    void publish( int tensor_fd, int acquire_fence_fd = -1, int64_t ts_us = -1 );

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
    void run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url );

    // Transport setup (accept + socket options / avio_open) and the
    // stream setup (new_stream + header + mpegts options).
    bool open_transport( const std::string& url, AVFormatContext* fc, Transport& t );
    bool setup_stream( AVFormatContext* fc, AVStream** st, bool tcp_pull );

    bool init_frames();
    void free_frames();
    int64_t packet_pts();  // pts of the next frame; may set ts_base_
    int mux_write( AVFormatContext* fc, PktPtr pkt );
    bool prep_frame( int tensor_fd, MppBuffer mb );

private:
    const int w_, h_, fps_, port_, bitrate_kbps_;
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
    // Three buffers, not two. Two worked only at a steady frame pace;
    // with --net-fps 0 (VFR) frames arrive in clumps after a network
    // pause, and two buffers can run out exactly at the 3-deep rkmpp
    // task queue while an older encode still holds its plane.
    enum { kBufCount = 3 };
    RkmppDrmPool<kBufCount> pool_;  // three ping-pong NV12 dma-bufs, one RAII sub-object
    uint32_t cur_ = 0;
    AVDRMFrameDescriptor desc_[kBufCount] {};  // per-buffer DRM descriptors (DRM_PRIME)
    std::array<AVFrame*, kBufCount> frames_ {};  // pre-bound DRM_PRIME frames (one per pool slot)

    // Encoder -> serve() packet queue. A separate mutex from mx_:
    // network-side wakeups never touch the encode critical section.
    std::mutex pkt_mx_;
    std::condition_variable pkt_cv_;
    std::deque<PktPtr> pkts_;
    int pkt_drops_ = 0;
    bool wait_key_ = true;  // pkt_mx_: drain drops packets until the next IDR (see net_encoder.cpp)
    std::atomic<bool> active_ { false };  // stream path live (serve sets/clears under mx_)
    int acq_fd_ = -1;          // draw fence fd: polled + closed in prep_frame()
                               // (called from publish, pipeline thread; serve() never touches it)
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
    uint64_t frames_sent_ = 0;
    int model_w_ = 0, model_h_ = 0;  // NPU tensor geometry (RGB888)
    int band_left_ = 0, band_top_ = 0;  // visible letterbox band in the tensor
    int band_w_ = 0, band_h_ = 0;       // band size (portrait: vertical strip)
    int vstride_ = 0;  // NV12 ver stride (16-aligned h), set in the ctor

    // h264 parameter-set prefix captured from the first key packet,
    // prepended to every following IDR (late-joiner support).
    // Plain members: function-local statics are forbidden outside tools/.
    std::vector< uint8_t > hdr_;
    bool hdr_logged_ = false;
};
