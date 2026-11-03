#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// DRM frame descriptor for the encoder zero-copy input: the ping-pong
// dma-bufs are wrapped as AV_PIX_FMT_DRM_PRIME frames and imported by
// the rkmpp wrapper by fd - no CPU pixel copies anywhere in the path.
#include <libavutil/hwcontext_drm.h>
// DRM_FORMAT_* constants (NV12, MOD_LINEAR) live in drm_fourcc.h;
// hwcontext_drm.h declares the descriptor struct but does not pull it in.
#include <libdrm/drm_fourcc.h>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>
#include <vector>

// Opaque ffmpeg types, forward-declared: the header compiles without
// the libav* include paths, and the compiler now checks the handle
// types at call sites instead of trusting comments.
typedef struct AVFormatContext AVFormatContext;
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket AVPacket;
typedef struct AVStream AVStream;

// M3.5 phase B2: hardware-encoded preview (h264_rkmpp) in MPEG-TS.
// Default transport is TCP pull: the board listens on net_port, the
// viewer connects when it wants (ffplay tcp://<board-ip>:1234). The
// legacy UDP push to net_host (--net-host) remains available: UDP has
// no "connect", the viewer joins whenever it wants, and a lost packet
// glitches briefly until the next IDR instead of stalling the whole
// flow like TCP over Wi-Fi.
class NetEncoder
{
public:
    NetEncoder( int w, int h, int fps, int port, const std::string& host, int bitrate_kbps, int model_w, int model_h );
    ~NetEncoder();

    // Called once per frame from the pipeline loop. tensor_fd is the
    // dma-buf of the NPU input tensor (RGB888, w x h); boxes are drawn.
    // M4: ts_us is the dequeue timestamp in microseconds from pipeline
    // start — the pts source in follow-source mode (--net-fps 0, VFR);
    // ignored in the fixed-fps mode.
    void publish( int tensor_fd, int acquire_fence_fd = -1, int64_t ts_us = -1 );

    // Batch C: true while the stream path is live (viewer connected in
    // PULL mode / UDP path open). The pipeline gates GPU box drawing
    // on it: recognition runs continuously, output does not.
    bool viewer_active() const { return active_.load(); }

private:
    void serve();
    bool send_frame( AVStream* st );
    bool prep_frame( int tensor_fd, MppBuffer mb );

    const int w_, h_, fps_, port_, bitrate_kbps_;
    const std::string host_;
    std::thread thread_;
    std::atomic<bool> quit_ { false };
    std::atomic<bool> error_ { false };

    std::mutex mx_;
    std::condition_variable cv_;
    AVFormatContext* fc_ = nullptr;  // mpegts: TCP pull or UDP push
    AVStream* st_ = nullptr;
    AVCodecContext* enc_ = nullptr;  // h264_rkmpp
    AVFrame* frame_ = nullptr;  // NV12 over an MPP DRM buffer
    AVPacket* pkt_ = nullptr;
    MppBufferGroup buf_group_ = nullptr;
    // P1-4: three buffers, not two. Two worked only at a steady frame
    // pace; with --net-fps 0 (VFR) frames arrive in clumps after a network
    // pause, and two buffers can run out exactly at the 3-deep rkmpp task
    // queue while an older encode still holds its plane.
    static constexpr int kBufCount = 3;
    MppBuffer bufs_[kBufCount] = { nullptr, nullptr, nullptr };  // ping-pong NV12 dma-bufs
    uint32_t cur_ = 0;
    AVDRMFrameDescriptor desc_[kBufCount] {};  // per-buffer DRM descriptors (DRM_PRIME)
    // Batch C: encoder -> serve() packet queue. A separate mutex from
    // mx_: network-side wakeups never touch the encode critical section.
    std::mutex pkt_mx_;
    std::condition_variable pkt_cv_;
    std::deque<AVPacket*> pkts_;
    int pkt_drops_ = 0;
    std::atomic<bool> active_ { false };  // stream path live (serve sets/clears under mx_)
    int acq_fd_ = -1;          // draw fence fd, consumed by RGA in send_frame
    int64_t frame_step_ = 1;   // pts increment in the muxer's time base
    uint64_t pts_ = 0;
    // M4 follow-source (--net-fps 0): pts stamped from the real dequeue
    // times. ts_us_ arrives with publish(); ts_base_ is the session start
    // (pts restart from zero when a new viewer connects). frames_sent_
    // gates the destructor drain in both modes (pts_ alone would be 0
    // in VFR and skip the flush).
    bool vfr_ = false;
    int64_t ts_us_ = -1;
    int64_t ts_base_ = -1;
    uint64_t frames_sent_ = 0;
    int model_w_ = 0, model_h_ = 0;  // NPU tensor geometry (RGB888)
    int band_top_ = 0, band_h_ = 0;  // visible letterbox band in the tensor
    int vstride_ = 0;  // NV12 ver stride (16-aligned h), set in the ctor

    // h264 parameter-set prefix captured from the first key
    // packet, prepended to every following IDR (late-joiner
    // support). Plain members: function-local statics are
    // forbidden outside tools/.
    std::vector< uint8_t > hdr_;
    bool hdr_logged_ = false;
};
