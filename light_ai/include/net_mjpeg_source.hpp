#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/byte_window.hpp"
#include "core/multipart_reader.hpp"

// HTTP MJPEG pull source (IP Webcam / any multipart/x-mixed-replace server).
// Media stays JPEG end to end: the MPP decoder consumes these
// packets exactly like the USB camera's MJPEG buffers -- no transcode anywhere.
// The only cost vs V4L2 is one memcpy per frame (socket -> slot);
// V4L2 hands the dma-buf zero-copy, HTTP cannot.

class NetMjpegSource
{
    enum
    {
        kBufCount = 4,
        kFrameMax = 1 << 20,  // 1 MiB per JPEG frame cap (cast to size_t at use sites)
        kSockBuf = 256 * 1024,  // the receive window: also the desync limit (no delimiter in it)
        kPollMs = 2000,      // per-fill watchdog, same window as the camera
        kErrStreakMax = 4,   // corrupt/short-frame streak tolerance
        kIntr = -3,  // net_fill: a signal interrupted poll/recv (transient, callers retry)
    };

    struct Buffer
    {
        void* start = nullptr;
        size_t length = 0;
        bool in_flight = false;
    };

public:
    NetMjpegSource();
    ~NetMjpegSource();
    NetMjpegSource( const NetMjpegSource& ) = delete;
    NetMjpegSource& operator=( const NetMjpegSource& ) = delete;

    // Full connect path for an http://host[:port]/path URL. Non-throwing:
    // 0 on success, non-zero fills *why.
    int setup_device( const std::string& url, std::string* why );
    void teardown();
    // Drains frames accumulated in the TCP socket during Pipeline init
    // so run() starts at the live edge alongside CameraV4L2::start().
    void start();

    // Returns the buffer index holding one complete JPEG frame;
    // size receives the frame length. On failure returns -1 with the errno
    // class in *err, same ladder as the camera: EIO = recoverable stall
    // (watchdog, hangup, corrupt-frame streak, desync),
    // ENODEV = closed by teardown, EAGAIN = no frame ready within timeout_ms.
    // Corrupt/incomplete frames are dropped with EAGAIN (or EIO once the
    // streak reaches kErrStreakMax); the streak survives across calls and
    // resets only on a clean frame or reconnect().

    int dequeue( uint32_t* size, int* err, int timeout_ms = -1 );
    void requeue( int index );
    void* buffer( int index ) const { return bufs_[index].start; }
    size_t buffer_size( int index ) const { return bufs_[index].length; }
    int width() const { return width_; }
    int height() const { return height_; }

    // One recovery-ladder step: re-open the URL. A URL never re-enumerates,
    // so new_id returns unchanged (the camera path adopts re-enumerated nodes there).
    // busy_nodes: ignored here -- a URL never re-enumerates.
    int recover( const std::string& id, std::string& new_id, const std::vector<std::string>& busy_nodes,
                 std::string* why );

private:
    int connect_tcp( const std::string& host, int port, std::string* why );
    int send_get( const std::string& host, int port, const std::string& path, std::string* why ) const;
    // Response head (core/http_head.hpp) -> the reader's boundary.
    int read_headers( std::string* why );
    // One complete part (or a wedged-full window) is buffered: no fill needed.
    bool part_buffered() const;
    // The desync protocol: the buffered window is dropped, the reader resyncs.
    void drop_window();
    int poll_part_ready( int timeout_ms );
    bool wait_part_ready( int timeout_ms, int* err );
    int read_latest_part( int slot, uint32_t* size );
    // One part into slot (core/multipart_reader.hpp): 0, -1 = EOF / error /
    // watchdog, -2 = desync.
    int read_part( int slot, uint32_t* size );
    // One poll+recv into the window. >0 = bytes read, 0 = timeout,
    // kIntr = a signal interrupted poll/recv (transient), -1 = EOF/error,
    // -2 = window wedged full (the desync protocol).
    int net_fill( int timeout_ms );

private:
    int fd_ = -1;
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    uint32_t primed_len_ = 0;  // first frame read during setup_device to extract SOF dims
    void* pool_ = nullptr;     // one mmap block: kBufCount x kFrameMax, init-time

    Buffer bufs_[kBufCount] = {};
    ByteWindow<kSockBuf> win_;
    MultipartReader parts_ { static_cast<size_t>( kFrameMax ) };  // the boundary is set per connect
};
