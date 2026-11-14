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

#include "pod_ring.hpp"

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>

#include "core/encode_pacer.hpp"     // EncodePacer: which publishes are encoded
#include "core/encoder_config.hpp"  // EncoderConfig, StreamLayout
#include "core/geometry.hpp"        // LbGeom: band geometry arrives as one object
#include "core/param_set_cache.hpp"  // ParamSetCache: late-join SPS/PPS repeat
#include "core/pts_clock.hpp"        // PtsClock: CFR / VFR pts
#include "net/encoder_frame_pool.hpp"  // EncoderFramePool: the encoder input ring
#include "rkmpp_drm_pool.hpp"  // RkmppDrmPool<1>: RAII canvas sub-objects
#include "tcp_pull_server.hpp" // TcpPullServer: RAII single-viewer TCP pull transport

class Log;
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
        kPktQDepth = 32
    };

public:
    // log: the process diagnostics, owned by main(); outlives the encoder
    // (the serve thread logs through it too: Log is thread-safe).
    // cfg: the stream geometry, rate and endpoint (core/encoder_config.hpp).
    NetEncoder( const EncoderConfig& cfg, const Log& log );

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
    // source; idle encodes share the pacer budget with live
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
    // packet with the IDR-prepend logic (params_).
    void drain_queue( AVFormatContext* fc );
    // One finished encoder packet -> the preallocated ring queue for
    // serve() to mux; shared by the EAGAIN retry and the send drain.
    void enqueue_pkt( AVPacket* pkt, AVStream* st, int64_t fpts );
    void run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url );

    // Transport setup (accept + socket options / avio_open) and the
    // stream setup (new_stream + header + mpegts options).
    bool open_transport( const std::string& url, AVFormatContext* fc, Transport& t );
    // frame_step: muxer ticks per codec pts, handed to pts_ under mx_ by run_session().
    bool setup_stream( AVFormatContext* fc, AVStream** st, int64_t* frame_step, bool tcp_pull );

    bool init_packets();
    void free_packets();
    void clear_pkt_queue();
    int queue_depth();  // packets queued for serve() (pkt_mx_)
    int mux_write( AVFormatContext* fc, AVPacket* pkt );
    bool prep_frame( int tensor_fd, const LbGeom& band, int src_idx, MppBuffer mb );

    // NO SIGNAL frame assembly (idle path): copies the idle-plate tile(s)
    // into the target buffer. False on RGA failure.
    bool prep_idle_frame( const bool* absent, int nabs, MppBuffer mb );

private:
    const Log& log_;
    const EncoderConfig cfg_;
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
    // The encoder's input ring: 4 NV12 dma-bufs pre-bound to DRM_PRIME
    // AVFrames (net/encoder_frame_pool.hpp). Freed after the codec (dtor body).
    EncoderFramePool ring_;
    RkmppDrmPool<1> canvas_;     // persistent composite canvas (multi-view)
    RkmppDrmPool<1> idle_pool_;  // pre-rendered NO SIGNAL canvas: gray + plate, stamped once at init
    // last band: ring slot (single-view) or canvas cell (multi-view)
    std::array<LbGeom, EncoderFramePool::kSlots + 1> buf_bands_ {};

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
    EncodePacer pacer_;  // which publishes are encoded (pipeline thread only)
    // CFR counter / VFR dequeue-time pts and the muxer step (core/pts_clock.hpp).
    // mx_: next() / advance() on the pipeline thread, restart() by the serve
    // thread at session start (pts restart from zero when a new viewer connects).
    PtsClock pts_;
    int64_t ts_us_ = -1;  // dequeue time of the frame being encoded (publish -> send_frame)
    // Bumped by the serve thread (under mx_) at every session start, before
    // active_; publish() snapshots it after its active_ check and the pacer
    // resets its budget on a new value. Replaces the cross-thread reset of
    // the former atomic last_enc_ts_us_: the pacer is pipeline-thread only.
    std::atomic<uint32_t> session_epoch_ { 0 };
    // Frames encoded; gates the destructor drain in both modes (the CFR
    // counter alone would be 0 in VFR and skip the flush).
    uint64_t frames_sent_ = 0;

    // Late-joiner support: the SPS/PPS prefix of the first IDR, repeated
    // on every later IDR (serve thread only).
    ParamSetCache params_;
};
