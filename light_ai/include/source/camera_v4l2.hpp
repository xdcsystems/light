#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/status.hpp"

class Log;
struct v4l2_buffer;  // <linux/videodev2.h>, included by the .cpp only

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
    // Nothing is trusted blindly: a mode that is not MJPEG or under 16 px is caught here, exactly like at startup.
    // Non-throwing: a failure tears down and returns its Status (Absent: the node
    // is not there; Busy; Unsupported: the mode; NoMemory: mmap); details are logged here.
    // Does not adopt the negotiated size: that is reconnect()'s, after STREAMON
    // (a trial() setup must not change the session mode).
    Status setup_device( const std::string& dev );

    // Release the fd and all mappings; safe on a partially-open or already-dead device
    // (STREAMOFF errors are ignored on purpose).
    void teardown() noexcept;

    // Full teardown + setup_steps() + STREAMON (+ adopting the mode): the only way the camera starts
    // streaming (startup, hot attach, re-attach -- SourceChannel::attach).
    // Not ok: the setup Status, or "STREAMON: <errno>" (the cause is logged here).
    Status reconnect( const std::string& dev );

    // ok: index = the buffer index, size = the frame length. Otherwise
    // dequeue_status( errno ) (source/source_types.hpp): Timeout = nothing
    // ready in timeout_ms; Lost = ENODEV / EIO, gone or a stall (recoverable);
    // anything else is a hard error.
    // Corrupt buffers (BUF_FLAG_ERROR) are counted ACROSS calls --
    // the err_streak_ member, reset only by a clean frame or by reconnect();
    // kBufCount in a row = recoverable stall.
    Status dequeue( int& index, uint32_t& size, int timeout_ms = -1 ) noexcept;
    void requeue( int index ) noexcept;
    void* buffer( int index ) const noexcept { return bufs_[index].start; }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t skip_parts_cnt() const noexcept { return 0; }
    int64_t last_cap_us() const noexcept { return last_cap_us_; }

private:
    // setup_device() with the negotiated mode in got_w/got_h (adopted by reconnect()).
    Status setup_steps( const std::string& dev, int& got_w, int& got_h );
    // The steps of setup_steps(), in this order. The Status steps return ok, or the
    // fail() result; they log the cause first, except the zero-buffer REQBUFS exit
    // of map_buffers(), which reports through its Status only. request_rate() cannot fail.
    Status open_node( const std::string& dev );
    // S_FMT + G_FMT + the mode check; got_w/got_h receive the negotiated mode.
    Status negotiate_format( int& got_w, int& got_h );
    // S_PARM 30 fps: best effort, never fatal.
    void request_rate();
    // REQBUFS + the count clamp + the QUERYBUF / mmap / QBUF loop.
    Status map_buffers();
    // The one error exit of the setup steps: teardown(), returns st.
    Status fail( const Status& st );

    // The steps of dequeue(), on its caller's thread (the frame thread), like
    // err_streak_. wait_clean: poll + DQBUF into buf until a clean buffer comes out;
    // 0, or the errno class for *err (poll_frame's EAGAIN / EIO, the DQBUF errno,
    // EIO once the corrupt streak reaches kBufCount).
    int wait_clean( v4l2_buffer& buf, int timeout_ms );
    // One poll + DQBUF of wait_clean: 0, an errno as above, or -1 (kDequeueAgain in camera_v4l2.cpp: DQBUF
    // EINTR / EAGAIN, or a corrupt buffer requeued below the stall threshold).
    int dequeue_once( v4l2_buffer& buf, int timeout_ms );
    // skip_to_newest: the drain-ahead -- at most kBufCount - 1 buffers that are
    // already done replace buf with the newest clean one; the others are requeued.
    void skip_to_newest( v4l2_buffer& buf );

    // Non-throwing ioctl wrapper: EINTR retry only, errno is the caller's error report.
    int xioctl_r( unsigned long req, void* arg ) const;

private:
    const Log& log_;
    int fd_ = -1;
    int err_streak_ = 0;
    int64_t last_cap_us_ = -1;
    int width_ = 0, height_ = 0;
    Buffer bufs_[kBufCount];
};
