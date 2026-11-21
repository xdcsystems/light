#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "core/preview_ring.hpp"
#include "core/status.hpp"

// Opaque ffmpeg types, forward-declared: the header compiles without
// the libav* include paths; the real includes stay in the .cpp.
struct AVFormatContext;
struct AVPacket;
struct AVStream;
class Log;
class TcpPullServer;

// Raw preview over TCP, non-blocking producer path.
// The frame IS the NPU input tensor (RGB888, model_w x model_h, gray letterbox bars).
// NUT muxer wraps the raw frames so ffplay/VLC open the URL with no extra options.
// One viewer at a time.
//
// Threads: serve() and its session steps (including av_write_frame / avio_flush)
// run on the preview thread; viewer_connected() and publish() run on the pipeline
// thread and communicate through the pre-allocated 2-slot drop-oldest PreviewRing.
// A stalled viewer drops frames in the ring without blocking the pipeline thread.

class NetPreview
{
public:
    // log: diagnostics, also from the preview thread (must outlive this object).
    NetPreview( int w, int h, int stride, int fps, int port, const Log& log );
    ~NetPreview();
    NetPreview( const NetPreview& ) = delete;
    NetPreview& operator=( const NetPreview& ) = delete;

    // True while a TCP viewer is attached. The pipeline gates GPU box
    // drawing on it: no viewer - no draw.
    bool viewer_connected() const;
    bool session_open() const noexcept;
    uint32_t session_epoch() const noexcept;

    // Called once per frame from the pipeline loop (same thread every time).
    // Copies the tensor frame into the 2-slot drop-oldest ring without waiting on socket I/O.
    void publish( const uint8_t* rgb ) noexcept;

private:
    // The preview thread: listen, then one viewer session after another.
    void serve() noexcept;
    // One session, in serve()'s order (preview thread). fc: the session's
    // muxer context, owned by serve()'s guard.
    Status open_session( TcpPullServer& tcp, AVFormatContext*& fc, AVStream*& st );
    uint64_t compute_frame_step( const AVStream* st ) const noexcept;
    void go_live() noexcept;
    bool drain_session( AVFormatContext* fc, AVStream* st, uint64_t frame_step ) noexcept;
    void close_session( AVFormatContext* fc, bool write_err );

private:
    const Log& log_;
    const int w_, h_, fps_, pace_fps_, port_;
    const size_t src_stride_;  // tensor row stride in bytes (>= w_ * 3); the ring holds packed rows
    PreviewRing ring_;
    std::vector<uint8_t> send_buf_;
    std::thread thread_;
    std::atomic<bool> quit_ { false };
    std::atomic<bool> connected_ { false };
    std::atomic<uint32_t> session_epoch_ { 0 };
    std::atomic<int64_t> next_send_us_ { 0 };

    std::mutex tcp_mx_;
    TcpPullServer* active_tcp_ = nullptr;
    AVPacket* pkt_ = nullptr;  // allocated once, used only on serve() thread
};
