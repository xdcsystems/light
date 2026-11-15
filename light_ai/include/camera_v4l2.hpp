#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class Log;

// The node is NOT part of the instance state: open/reconnect take it as
// an argument, the class owns only the fd/buffer machinery. Identity
// ("which node am I on") and node discovery live with the owner --
// SourceChannel::node() and UsbNodeLocator (source/).
class CameraV4L2
{
    struct Buffer
    {
        void* start = nullptr;
        size_t length = 0;
    };

    enum : unsigned
    {
        kBufCount = 4,
        kDequeuePollMs = 2000,  // dequeue watchdog: no frame within this window = recoverable stall (EIO)
        kDefaultWidth = 1280,
        kDefaultHeight = 960
    };

public:
    explicit CameraV4L2( const Log& log ) : log_( log ) {}
    ~CameraV4L2();
    CameraV4L2( const CameraV4L2& ) = delete;
    CameraV4L2& operator=( const CameraV4L2& ) = delete;

    // Full open+verify path for a node:
    // open, S_FMT, G_FMT verification, REQBUFS, mmap, QBUF.
    // Nothing is trusted blindly: a substituted camera mode is caught here exactly like at startup. 
    // Non-throwing: failures return non-zero and fill *why (if not null); details are logged here.
    int setup_device( const std::string& dev, std::string* why );

    // Release the fd and all mappings; safe on a partially-open or already-dead device
    // (STREAMOFF errors are ignored on purpose).
    void teardown();

    // Full teardown + setup_device() + STREAMON: the only way the camera starts
    // streaming (startup, hot attach, re-attach -- SourceChannel::attach).
    // Returns 0 on success, non-zero with *why (the cause is logged here).
    int reconnect( const std::string& dev, std::string* why );

    // Returns the buffer index; size (if not null) receives the frame length.
    // On failure returns -1 and stores the errno value in *err:
    // ENODEV/EIO mean the device is gone or interrupted (recoverable);
    // any other errno is a hard error.
    // Corrupt buffers (BUF_FLAG_ERROR) are counted ACROSS calls --
    // the err_streak_ member, reset only by a clean frame or by reconnect();
    // kBufCount in a row = recoverable stall.
    int dequeue( uint32_t* size, int* err, int timeout_ms = -1 );
    void requeue( int index );
    void* buffer( int index ) const { return bufs_[index].start; }
    size_t buffer_size( int index ) const { return bufs_[index].length; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    // The steps of setup_device(), in this order. The int steps return 0, or the
    // fail() result; they log the cause first, except the zero-buffer REQBUFS exit
    // of map_buffers(), which reports through *why only. request_rate() cannot fail.
    int open_node( const std::string& dev, std::string* why );
    // S_FMT + G_FMT + the mode check; got_w/got_h receive the negotiated mode.
    int negotiate_format( int& got_w, int& got_h, std::string* why );
    // S_PARM 30 fps: best effort, never fatal.
    void request_rate();
    // REQBUFS + the count clamp + the QUERYBUF / mmap / QBUF loop.
    int map_buffers( std::string* why );
    // The one error exit of the setup steps: *why = text (if why is not null),
    // teardown(), returns -1.
    int fail( std::string* why, const char* text );

    // Non-throwing ioctl wrapper: EINTR retry only, errno is the caller's error report.
    int xioctl_r( unsigned long req, void* arg ) const;

private:
    const Log& log_;
    int fd_ = -1;
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    Buffer bufs_[kBufCount];
};
