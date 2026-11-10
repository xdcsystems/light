#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// Opaque ffmpeg types, forward-declared: the header compiles without
// the libav* include paths; the real includes stay in the .cpp.
struct AVFormatContext;
struct AVPacket;
struct AVStream;

// Raw preview over TCP, zero pixel copies.
// The frame IS the NPU input tensor (RGB888, model_w x model_h, gray letterbox bars).
// Its memory is wrapped into an ffmpeg AVBufferRef with a no-op free callback:
// av_write_frame sends the tensor bytes directly, no per-frame allocations, no pixel copies.
// NUT muxer wraps the raw frames so ffplay/VLC open the URL
// with no extra options. One viewer at a time.
// The connection opens lazily in a background thread: until a viewer connects,
// publish() is a no-op. A stalled viewer backpressures the pipeline
// (write blocks up to the socket I/O timeout, then the connection is dropped and
// the server waits for a new one).

class NetPreview
{
public:
    NetPreview( int w, int h, int stride, int fps, int port );
    ~NetPreview();
    NetPreview( const NetPreview& ) = delete;
    NetPreview& operator=( const NetPreview& ) = delete;

    // True while a TCP viewer is attached. The pipeline gates GPU box
    // drawing on it: no viewer - no draw.
    bool viewer_connected() const;

    // Called once per frame from the pipeline loop (same thread every time).
    // rgb points into the NPU input tensor; the call may block while the
    // frame drains to the socket.
    void publish( const uint8_t* rgb );

private:
    void serve();

private:
    const int w_, h_, stride_, fps_, pace_fps_, port_;
    std::thread thread_;
    std::atomic<bool> quit_ { false };
    std::atomic<bool> connected_ { false };
    std::atomic<bool> error_ { false };
    std::atomic<int64_t> next_send_us_ { 0 };

    std::mutex mx_;
    std::condition_variable cv_;
    AVFormatContext* fc_ = nullptr;  // ready for writes
    AVStream* stream_ = nullptr;  // of the video stream
    AVPacket* pkt_ = nullptr;  // allocated once
    uint64_t frame_step_ = 1;  // pts increment in the muxer's time base
    uint64_t pts_ = 0;
};
