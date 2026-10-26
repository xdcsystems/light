#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>


// M3.5 phase A0: raw preview over TCP, zero pixel copies.
//
// The frame IS the NPU input tensor (RGB888, model_w x model_h, gray
// letterbox bars). Its memory is wrapped into an ffmpeg AVBufferRef with a
// no-op free callback: av_write_frame sends the tensor bytes directly, no
// per-frame allocations, no pixel copies. NUT muxer wraps the raw frames so
// ffplay/VLC open the URL with no extra options. One viewer at a time.
//
// The connection opens lazily in a background thread: until a viewer
// connects, publish() is a no-op. A stalled viewer backpressures the
// pipeline (write blocks up to the socket I/O timeout, then the connection
// is dropped and the server waits for a new one).
class NetPreview {
public:
    NetPreview(int w, int h, int stride, int fps, int port);
    ~NetPreview();

    // Called once per frame from the pipeline loop (same thread every time).
    // rgb points into the NPU input tensor; the call may block while the
    // frame drains to the socket.
    void publish(const uint8_t* rgb);

private:
    void serve();

private:
    const int w_, h_, stride_, fps_, port_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> error_{false};

    // ffmpeg types hidden behind void*: the header stays free of the
    // ffmpeg include path (only net_preview.cpp sees the real structs).
    std::mutex mx_;
    std::condition_variable cv_;
    void* fc_ = nullptr;      // AVFormatContext*, ready for writes
    void* stream_ = nullptr;  // AVStream* of the video stream
    void* pkt_ = nullptr;     // AVPacket*, allocated once
    uint64_t frame_step_ = 1;   // pts increment in the muxer's time base
    uint64_t pts_ = 0;
};

