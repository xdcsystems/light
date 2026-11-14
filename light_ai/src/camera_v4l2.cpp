#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "camera_v4l2.hpp"
#include "core/log.hpp"

namespace
{
    // Single poll(2) call without an internal EINTR loop: on SIGINT/SIGTERM
    // SignalWaiter sets the stop flag and wakes this thread, so poll() returns
    // -1 (EINTR) immediately instead of restarting a new 2000 ms wait;
    // non-stop EINTR is retried by the pipeline loop.
    int poll_ready( int fd, int timeout_ms )
    {
        pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        return ::poll( &pfd, 1, timeout_ms );
    }

    // Returns 0 when a frame is ready to dequeue, or the errno class
    // (EAGAIN on non-blocking poll timeout, EIO on watchdog stall).
    int poll_frame( int fd, int timeout_ms, int default_ms, const Log& log )
    {
        const int wait_ms = timeout_ms < 0 ? default_ms : timeout_ms;
        const int pr = poll_ready( fd, wait_ms );
        if ( pr > 0 )
            return 0;
        if ( pr < 0 )
            return errno;
        if ( timeout_ms < 0 )
            log.warn() << "cam: no frame in " << default_ms << " ms - treating as recoverable stall\n";
        return timeout_ms >= 0 ? EAGAIN : EIO;
    }

}  // namespace

// Full open+setup path for the node given by the owner: open + S_FMT +
// G_FMT verification + REQBUFS/mmap/QBUF. Nothing is trusted blindly: a
// substituted camera mode is caught here exactly like at startup.
// Self-cleaning (P1-5): every error path after open() tears the device
// down, so a failed setup never leaks an fd or a mapping.
// Non-throwing: failures are reported via the return value and logged
// here.
int CameraV4L2::setup_device( const std::string& dev, std::string* why )
{
    // Idempotent entry: the startup fallback calls setup_device() twice
    // (direct setup first, then discovery re-runs it). Without this call
    // a failed direct setup leaks its fd and leaves dead buffer pointers
    // behind, which the following setup would silently overwrite;
    // teardown() is a no-op when nothing is open.
    teardown();

    fd_ = ::open( dev.c_str(), O_RDWR | O_CLOEXEC );
    if ( fd_ < 0 )
    {
        log_.warn() << "open camera: " << strerror( errno ) << '\n';
        if ( why )
            *why = "open failed";
        return -1;
    }

    const unsigned req_w = width_ > 0 ? static_cast<unsigned>( width_ ) : kDefaultWidth;
    const unsigned req_h = height_ > 0 ? static_cast<unsigned>( height_ ) : kDefaultHeight;
    v4l2_format fmt {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = req_w;
    fmt.fmt.pix.height = req_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if ( xioctl_r( VIDIOC_S_FMT, &fmt ) < 0 )
    {
        log_.warn() << "camera: S_FMT: " << strerror( errno ) << '\n';
        if ( why )
            *why = "S_FMT failed";
        teardown();
        return -1;
    }

    // Read back the actual mode negotiated by the driver (VIDIOC_G_FMT).
    // At startup (width_ == 0) we adopt the device's negotiated MJPEG
    // dimensions; on a mid-run reconnect (width_ > 0) the dimensions are
    // already locked into the decoder/encoder and must match.
    v4l2_format gfmt {};
    gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( xioctl_r( VIDIOC_G_FMT, &gfmt ) < 0 )
    {
        log_.warn() << "camera: G_FMT: " << strerror( errno ) << '\n';
        if ( why )
            *why = "G_FMT failed";
        teardown();
        return -1;
    }
    const int got_w = static_cast<int>( gfmt.fmt.pix.width );
    const int got_h = static_cast<int>( gfmt.fmt.pix.height );
    if ( gfmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG || got_w < 16 || got_h < 16
         || ( width_ > 0 && ( got_w != width_ || got_h != height_ ) ) )
    {
        log_.warn() << "camera: mode mismatch: requested " << req_w << 'x' << req_h << " MJPEG, got " << got_w << 'x'
                  << got_h << " fourcc=" << std::hex << gfmt.fmt.pix.pixelformat << std::dec << '\n';
        if ( why )
            *why = "camera mode mismatch";
        teardown();
        return -1;
    }

    // Ask explicitly for the highest declared interval. The Sunplus FW
    // accepts this request and honors it when AE picks a short exposure;
    // in dim light it still throttles to its own rhythm — the request is
    // harmless and documents intent. Best-effort: never fatal.
    v4l2_streamparm parm {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if ( xioctl_r( VIDIOC_S_PARM, &parm ) == 0 )
    {
        const unsigned d = parm.parm.capture.timeperframe.denominator;
        if ( d != 30 )
            log_.warn() << "camera: requested 30 fps, driver set " << d << "\n";
    }

    v4l2_requestbuffers req {};
    req.count = kBufCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if ( xioctl_r( VIDIOC_REQBUFS, &req ) < 0 )
    {
        log_.warn() << "camera: REQBUFS: " << strerror( errno ) << '\n';
        if ( why )
            *why = "REQBUFS failed";
        teardown();
        return -1;
    }

    if ( req.count == 0 )
    {
        if ( why )
            *why = "camera: REQBUFS returned zero buffers";
        teardown();
        return -1;
    }

    // V4L2 allows the driver to return MORE buffers than requested
    // (VIDIOC_REQBUFS spec). The mmap/QBUF loops below index bufs_[],
    // which has kBufCount slots — clamp before touching them.
    if ( req.count > kBufCount )
        req.count = kBufCount;
    // The driver may also return FEWER buffers than requested.
    // With < 3 the pipeline loses its ping-pong depth (latency and jitter grow)
    // - say so instead of silently degrading.
    if ( req.count < 3 )
        log_.warn() << "camera: REQBUFS returned only " << req.count
                  << " buffers - expect extra latency\n";

    for ( unsigned i = 0; i < req.count; ++i )
    {
        v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if ( xioctl_r( VIDIOC_QUERYBUF, &buf ) < 0 )
        {
            log_.warn() << "camera: QUERYBUF idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "QUERYBUF failed";
            teardown();
            return -1;
        }
        bufs_[i].length = buf.length;
        bufs_[i].start = mmap( nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset );
        if ( bufs_[i].start == reinterpret_cast<void *>( -1 ) )  // MAP_FAILED: its ((void *) -1) lives in a system header
        {
            bufs_[i].start = nullptr;
            log_.warn() << "camera: mmap idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "mmap failed";
            teardown();
            return -1;
        }
        if ( xioctl_r( VIDIOC_QBUF, &buf ) < 0 )
        {
            log_.warn() << "camera: QBUF idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "QBUF failed";
            teardown();
            return -1;
        }
    }
    width_ = got_w;
    height_ = got_h;
    return 0;
}

// Release the fd and all mappings. Safe on a partially-open device and
// on an already-dead one: STREAMOFF errors are ignored on purpose
// (the fd may not exist anymore), unmapped slots are skipped via the
// start==nullptr guard.
void CameraV4L2::teardown()
{
    if ( fd_ >= 0 )
    {
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl( fd_, VIDIOC_STREAMOFF, &t );
        close( fd_ );
        fd_ = -1;
    }
    // Do not stop at the first empty slot: slot indices are dense
    // today, but a future "skip a slot" logic in setup would silently leak
    // every mapping after it. Skip empty slots instead of breaking out.
    for ( auto& b : bufs_ )
    {
        if ( !b.start )
            continue;
        munmap( b.start, b.length );
        b.start = nullptr;
        b.length = 0;
    }
}

// Full teardown + the setup path + STREAMON: the one way a camera starts
// streaming (startup, hot attach, re-attach). The node is supplied by the
// owner (UsbNodeLocator) -- the class never decides which node to open.
int CameraV4L2::reconnect( const std::string& dev, std::string* why )
{
    teardown();
    if ( setup_device( dev, why ) != 0 )
    {
        teardown();
        return -1;
    }
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( xioctl_r( VIDIOC_STREAMON, &t ) < 0 )
    {
        const std::string err = strerror( errno );  // before the log write can touch errno
        log_.warn() << "camera: reconnect STREAMON: " << err << '\n';
        if ( why )
            *why = "STREAMON: " + err;
        teardown();
        return -1;
    }
    err_streak_ = 0;  // fresh device session (stream running) — the corrupt streak starts over
    return 0;
}

CameraV4L2::~CameraV4L2()
{
    teardown();
}

int CameraV4L2::xioctl_r( unsigned long req, void* arg ) const
{
    int r = 0;
    do
    {
        r = ioctl( fd_, req, arg );
    }
    while ( r < 0 && errno == EINTR );
    return r;
}

int CameraV4L2::dequeue( uint32_t* size, int* err, int timeout_ms )  // NOLINT(readability-make-member-function-const)
{
    v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    bool have = false;  // loop until a clean buffer comes out
    while ( !have )
    {
        // Watchdog: the wait is bounded by poll(). In single-source mode
        // (timeout_ms < 0) a 2 s timeout is reported as EIO (recoverable
        // stall); in multi-source non-blocking mode (timeout_ms >= 0) a
        // 0/short poll timeout returns EAGAIN so the dispatcher can
        // service other ready cameras immediately.
        if ( const int perr = poll_frame( fd_, timeout_ms, static_cast<int>( kDequeuePollMs ), log_ ); perr != 0 )
        {
            if ( err )
                *err = perr;
            return -1;
        }
        if ( ioctl( fd_, VIDIOC_DQBUF, &buf ) < 0 )
        {
            if ( errno == EINTR || errno == EAGAIN )
                continue;
            if ( err )
                *err = errno;
            return -1;
        }
        if ( buf.flags & V4L2_BUF_FLAG_ERROR )
        {
            requeue( static_cast<int>( buf.index ) );
            // the streak survives dequeue() calls
            // (e.g. corrupt frames followed by a poll timeout),
            // so the watchdog threshold reflects the real buffer history.
            ++err_streak_;
            if ( err_streak_ < static_cast<int>( kBufCount ) )
                continue;
            log_.warn() << "cam: " << err_streak_ << " consecutive corrupt buffers - stall\n";
            if ( err )
                *err = EIO;
            return -1;
        }
        have = true;
    }
    for ( unsigned skip = 0; skip + 1 < kBufCount && poll_ready( fd_, 0 ) > 0; ++skip )
    {
        v4l2_buffer next_buf {};
        next_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        next_buf.memory = V4L2_MEMORY_MMAP;
        if ( ioctl( fd_, VIDIOC_DQBUF, &next_buf ) < 0 )
            break;
        if ( ( next_buf.flags & V4L2_BUF_FLAG_ERROR ) || next_buf.bytesused == 0 )
        {
            requeue( static_cast<int>( next_buf.index ) );
            continue;
        }
        requeue( static_cast<int>( buf.index ) );
        buf = next_buf;
    }
    err_streak_ = 0;  // clean buffer resets the streak
    if ( err )
        *err = 0;
    if ( size )
        *size = buf.bytesused;
    return buf.index;
}

void CameraV4L2::requeue( int index )  // NOLINT(readability-make-member-function-const)
{
    v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if ( ioctl( fd_, VIDIOC_QBUF, &buf ) < 0 )
    {
        // A failed QBUF silently removes the buffer from rotation;
        // without this log the symptom is only a mysterious cam-time growth.
        log_.warn() << "camera: QBUF failed for idx=" << index << ": " << strerror( errno ) << '\n';
    }
}
