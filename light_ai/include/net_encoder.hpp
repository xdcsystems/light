#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
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

// M3.5 phase B2: hardware-encoded preview (h264_rkmpp) in MPEG-TS over
// UDP. The stream is pushed to a fixed viewer address (net_host) as soon
// as the pipeline starts: UDP has no "connect", the viewer joins whenever
// it wants (VLC: udp://@:1234). A lost packet glitches briefly until the
// next IDR instead of stalling the whole flow like TCP over Wi-Fi.
class NetEncoder
{
public:
    NetEncoder( int w, int h, int fps, int port, const std::string& host, int bitrate_kbps, int model_w, int model_h );
    ~NetEncoder();

    // Called once per frame from the pipeline loop. tensor_fd is the
    // dma-buf of the NPU input tensor (RGB888, w x h); boxes are drawn.
    void publish( int tensor_fd, int acquire_fence_fd = -1 );

private:
    void serve();
    bool send_frame( void* fc, void* st );

    const int w_, h_, fps_, port_, bitrate_kbps_;
    const std::string host_;
    std::thread thread_;
    std::atomic<bool> quit_ { false };
    std::atomic<bool> error_ { false };

    std::mutex mx_;
    std::condition_variable cv_;
    void* fc_ = nullptr;     // AVFormatContext* (mpegts over UDP)
    void* st_ = nullptr;     // AVStream*
    void* enc_ = nullptr;    // AVCodecContext* (h264_rkmpp)
    void* frame_ = nullptr;  // AVFrame* (NV12 over an MPP DRM buffer)
    void* pkt_ = nullptr;    // AVPacket*
    MppBufferGroup buf_group_ = nullptr;
    MppBuffer bufs_[2] = { nullptr, nullptr };  // ping-pong NV12 dma-bufs
    uint32_t cur_ = 0;
    AVDRMFrameDescriptor desc_[2] {};  // per-buffer DRM descriptors (DRM_PRIME)
    int tensor_fd_ = -1;       // dma-buf fd of the NPU tensor (set in publish)
    int acq_fd_ = -1;          // draw fence fd, consumed by RGA in send_frame
    int64_t frame_step_ = 1;   // pts increment in the muxer's time base
    uint64_t pts_ = 0;
    int model_w_ = 0, model_h_ = 0;  // NPU tensor geometry (RGB888)
    int band_top_ = 0, band_h_ = 0;  // visible letterbox band in the tensor
    int vstride_ = 0;  // NV12 ver stride (16-aligned h), set in the ctor
};
