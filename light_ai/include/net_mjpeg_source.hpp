#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/multipart_reader.hpp"
#include "source/buffered_socket.hpp"
#include "source/mjpeg_slot_pool.hpp"

class Log;

// HTTP MJPEG pull source (IP Webcam / any multipart/x-mixed-replace server).
// Media stays JPEG end to end: the MPP decoder consumes these
// packets exactly like the USB camera's MJPEG buffers -- no transcode anywhere.
// The only cost vs V4L2 is one memcpy per frame (socket -> slot);
// V4L2 hands the dma-buf zero-copy, HTTP cannot.

class NetMjpegSource
{
    enum
    {
        kPollMs = 2000,     // per-fill watchdog, same window as the camera
        kErrStreakMax = 4,  // corrupt/short-frame streak tolerance
    };

public:
    // Throws when the frame pool cannot be mapped (MjpegSlotPool): the
    // startup path only (MediaSource::use_net <- SourceChannel::configure).
    explicit NetMjpegSource( const Log& log ) : log_( log ) {}
    ~NetMjpegSource() = default;  // the socket closes, the pool unmaps
    NetMjpegSource( const NetMjpegSource& ) = delete;
    NetMjpegSource& operator=( const NetMjpegSource& ) = delete;
    NetMjpegSource( NetMjpegSource&& ) = delete;
    NetMjpegSource& operator=( NetMjpegSource&& ) = delete;

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
    void* buffer( int index ) const { return pool_.data( index ); }
    size_t buffer_size( int ) const { return pool_.capacity(); }
    int width() const { return width_; }
    int height() const { return height_; }

private:
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
    // watchdog, -2 = desync (a stream that does not parse, or a wedged-full window).
    int read_part( int slot, uint32_t* size );

private:
    const Log& log_;
    MjpegSlotPool pool_;  // first: a failed mmap throws before anything else exists
    BufferedSocket sock_;
    MultipartReader parts_ { static_cast<size_t>( MjpegSlotPool::kSlotBytes ) };  // the boundary is set per connect
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    uint32_t primed_len_ = 0;  // first frame read during setup_device to extract SOF dims
};
