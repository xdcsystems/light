#include <cerrno>
#include <chrono>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "source/camera_v4l2.hpp"
#include "core/log.hpp"
#include "source/source_types.hpp"  // dequeue_status

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
        const int pr = ::poll( &pfd, 1, timeout_ms );
        if ( pr > 0 && ( pfd.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
        {
            errno = ( pfd.revents & POLLNVAL ) != 0 ? ENODEV : EIO;
            return -1;
        }
        return pr;
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

    // dequeue_once(): no clean buffer yet, poll again (never an errno: those are positive).
    constexpr int kDequeueAgain = -1;

    // A failed open / setup ioctl: the node gone or never there is Absent (the
    // locator moves on), EBUSY is Busy, anything else Failed.
    Status setup_errno( const char* what, int e )
    {
        Errc c = Errc::Failed;
        switch ( e )
        {
            case ENOENT:
            case ENODEV:
            case ENXIO: c = Errc::Absent; break;
            case EBUSY: c = Errc::Busy; break;
            default: break;
        }
        return errno_status( c, what, e );
    }

}  // namespace

// Full open+setup path for the node given by the owner: open + S_FMT +
// G_FMT verification + REQBUFS/mmap/QBUF. Nothing is trusted blindly: a
// substituted camera mode is caught here exactly like at startup.
// Self-cleaning (P1-5): every error path goes through fail(), which tears
// the device down, so a failed setup never leaks an fd or a mapping.
// Non-throwing: failures are reported via the returned Status and logged
// here.
Status CameraV4L2::setup_device( const std::string& dev )
{
    int got_w = 0;
    int got_h = 0;
    return setup_steps( dev, got_w, got_h );
}

Status CameraV4L2::setup_steps( const std::string& dev, int& got_w, int& got_h )
{
    // Idempotent entry: the startup fallback calls setup_device() twice
    // (direct setup first, then discovery re-runs it). Without this call
    // a failed direct setup leaks its fd and leaves dead buffer pointers
    // behind, which the following setup would silently overwrite;
    // teardown() is a no-op when nothing is open.
    teardown();

    got_w = 0;
    got_h = 0;
    if ( Status st = open_node( dev ); !st.ok() )
        return st;
    if ( Status st = negotiate_format( got_w, got_h ); !st.ok() )
        return st;
    request_rate();
    if ( Status st = map_buffers(); !st.ok() )
        return st;
    // Not adopted here (M-15): a trial() setup is torn down right after, and
    // reconnect() adopts only once STREAMON succeeded -- any failed attempt
    // leaves the session dimensions (the next request) as they were.
    return {};
}

Status CameraV4L2::fail( const Status& st )
{
    teardown();
    return st;
}

// open() failure: nothing is open yet, the teardown() inside fail() is a no-op.
Status CameraV4L2::open_node( const std::string& dev )
{
    fd_ = ::open( dev.c_str(), O_RDWR | O_CLOEXEC );
    if ( fd_ >= 0 )
        return {};
    const int e = errno;  // before the log write can touch errno
    log_.warn() << "open camera: " << ErrnoText { e } << '\n';
    return fail( setup_errno( "open failed", e ) );
}

// Read back the actual mode negotiated by the driver (VIDIOC_G_FMT).
// The request is the default mode at startup (width_ == 0) and the last
// session's mode on a reconnect; any negotiated MJPEG mode is accepted and
// reconnect() adopts it after STREAMON (a camera back in another mode
// re-attaches: SourceChannel re-initializes the decoder).
Status CameraV4L2::negotiate_format( int& got_w, int& got_h )
{
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
        const int e = errno;
        log_.warn() << "camera: S_FMT: " << ErrnoText { e } << '\n';
        return fail( setup_errno( "S_FMT failed", e ) );
    }

    v4l2_format gfmt {};
    gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( xioctl_r( VIDIOC_G_FMT, &gfmt ) < 0 )
    {
        const int e = errno;
        log_.warn() << "camera: G_FMT: " << ErrnoText { e } << '\n';
        return fail( setup_errno( "G_FMT failed", e ) );
    }
    got_w = static_cast<int>( gfmt.fmt.pix.width );
    got_h = static_cast<int>( gfmt.fmt.pix.height );
    if ( gfmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG || got_w < 16 || got_h < 16 )
    {
        log_.warn() << "camera: mode mismatch: requested " << req_w << 'x' << req_h << " MJPEG, got " << got_w << 'x'
                  << got_h << " fourcc=" << std::hex << gfmt.fmt.pix.pixelformat << std::dec << '\n';
        return fail( make_status( Errc::Unsupported, "camera mode mismatch" ) );
    }
    return {};
}

// Ask explicitly for the highest declared interval. The Sunplus FW
// accepts this request and honors it when AE picks a short exposure;
// in dim light it still throttles to its own rhythm — the request is
// harmless and documents intent. Best-effort: never fatal.
void CameraV4L2::request_rate()
{
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
}

Status CameraV4L2::map_buffers()
{
    v4l2_requestbuffers req {};
    req.count = kBufCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if ( xioctl_r( VIDIOC_REQBUFS, &req ) < 0 )
    {
        const int e = errno;
        log_.warn() << "camera: REQBUFS: " << ErrnoText { e } << '\n';
        return fail( setup_errno( "REQBUFS failed", e ) );
    }

    if ( req.count == 0 )
        return fail( make_status( Errc::Unsupported, "camera: REQBUFS returned zero buffers" ) );

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
            const int e = errno;
            log_.warn() << "camera: QUERYBUF idx=" << i << ": " << ErrnoText { e } << '\n';
            return fail( setup_errno( "QUERYBUF failed", e ) );
        }
        bufs_[i].length = buf.length;
        bufs_[i].start = mmap( nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset );
        if ( bufs_[i].start == reinterpret_cast<void *>( -1 ) )  // MAP_FAILED: its ((void *) -1) lives in a system header
        {
            const int e = errno;
            bufs_[i].start = nullptr;
            log_.warn() << "camera: mmap idx=" << i << ": " << ErrnoText { e } << '\n';
            return fail( errno_status( Errc::NoMemory, "mmap failed", e ) );
        }
        if ( xioctl_r( VIDIOC_QBUF, &buf ) < 0 )
        {
            const int e = errno;
            log_.warn() << "camera: QBUF idx=" << i << ": " << ErrnoText { e } << '\n';
            return fail( setup_errno( "QBUF failed", e ) );
        }
    }
    return {};
}

// Release the fd and all mappings. Safe on a partially-open device and
// on an already-dead one: STREAMOFF errors are ignored on purpose
// (the fd may not exist anymore), unmapped slots are skipped via the
// start==nullptr guard.
void CameraV4L2::teardown() noexcept
{
    if ( fd_ >= 0 )
    {
        v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl( fd_, VIDIOC_STREAMOFF, &t );
    }
    // Unmap before close (Low §2): the mappings belong to the open file;
    // releasing them first is the V4L2 teardown order (STREAMOFF, munmap,
    // close) and frees the driver buffers on close instead of on the last
    // munmap. Do not stop at the first empty slot: slot indices are dense
    // today, but a future "skip a slot" logic in setup would silently leak
    // every mapping after it.
    for ( auto& b : bufs_ )
    {
        if ( !b.start )
            continue;
        munmap( b.start, b.length );
        b.start = nullptr;
        b.length = 0;
    }
    if ( fd_ >= 0 )
    {
        close( fd_ );
        fd_ = -1;
    }
}

// Full teardown + the setup path + STREAMON: the one way a camera starts
// streaming (startup, hot attach, re-attach). The node is supplied by the
// owner (UsbNodeLocator) -- the class never decides which node to open.
Status CameraV4L2::reconnect( const std::string& dev )
{
    teardown();
    int got_w = 0;
    int got_h = 0;
    if ( Status st = setup_steps( dev, got_w, got_h ); !st.ok() )
    {
        teardown();
        return st;
    }
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( xioctl_r( VIDIOC_STREAMON, &t ) < 0 )
    {
        const int e = errno;  // before the log write can touch errno
        log_.warn() << "camera: reconnect STREAMON: " << ErrnoText { e } << '\n';
        teardown();
        return setup_errno( "STREAMON", e );
    }
    // The stream runs: the negotiated mode becomes the session's.
    width_ = got_w;
    height_ = got_h;
    err_streak_ = 0;  // fresh device session (stream running) — the corrupt streak starts over
    return {};
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

Status CameraV4L2::dequeue( int& index, uint32_t& size, int timeout_ms ) noexcept  // NOLINT(readability-make-member-function-const)
{
    v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if ( const int werr = wait_clean( buf, timeout_ms ); werr != 0 )
        return dequeue_status( werr );
    skip_to_newest( buf );
    if ( ( buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK ) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
         ( buf.timestamp.tv_sec > 0 || buf.timestamp.tv_usec > 0 ) )
    {
        last_cap_us_ = static_cast<int64_t>( buf.timestamp.tv_sec ) * 1000000LL +
                       static_cast<int64_t>( buf.timestamp.tv_usec );
    }
    else
    {
        last_cap_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch() )
                           .count();
    }
    err_streak_ = 0;  // clean buffer resets the streak
    size = buf.bytesused;
    index = static_cast<int>( buf.index );
    return {};
}

// Loop until a clean buffer comes out, bounded by deadline and attempt limit.
int CameraV4L2::wait_clean( v4l2_buffer& buf, int timeout_ms )
{
    const auto wait_ms = timeout_ms >= 0 ? std::max( timeout_ms, 50 ) : static_cast<int>( kDequeuePollMs );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( wait_ms );
    constexpr int max_attempts = static_cast<int>( kBufCount ) * 4 + 8;
    int rc = 0;
    for ( int attempt = 0; attempt < max_attempts; ++attempt )
    {
        rc = dequeue_once( buf, timeout_ms );
        if ( rc != kDequeueAgain )
            return rc;
        if ( std::chrono::steady_clock::now() >= deadline )
            return timeout_ms >= 0 ? EAGAIN : EIO;
    }
    return timeout_ms >= 0 ? EAGAIN : EIO;
}

int CameraV4L2::dequeue_once( v4l2_buffer& buf, int timeout_ms )
{
    // Watchdog: the wait is bounded by poll(). In single-source mode
    // (timeout_ms < 0) a 2 s timeout is reported as EIO (recoverable
    // stall); in multi-source non-blocking mode (timeout_ms >= 0) a
    // 0/short poll timeout returns EAGAIN so the dispatcher can
    // service other ready cameras immediately.
    if ( const int perr = poll_frame( fd_, timeout_ms, static_cast<int>( kDequeuePollMs ), log_ ); perr != 0 )
        return perr;
    if ( ioctl( fd_, VIDIOC_DQBUF, &buf ) < 0 )
    {
        if ( errno == EAGAIN && timeout_ms >= 0 )
            return EAGAIN;
        return ( errno == EINTR || errno == EAGAIN ) ? kDequeueAgain : errno;
    }
    if ( !( buf.flags & V4L2_BUF_FLAG_ERROR ) && buf.bytesused > 0 )
        return 0;
    requeue( static_cast<int>( buf.index ) );
    // the streak survives dequeue() calls
    // (e.g. corrupt frames followed by a poll timeout),
    // so the watchdog threshold reflects the real buffer history.
    ++err_streak_;
    if ( err_streak_ >= static_cast<int>( kBufCount ) )
    {
        log_.warn() << "cam: " << err_streak_ << " consecutive corrupt buffers - stall\n";
        return EIO;
    }
    return kDequeueAgain;
}

// Buffers that completed while the frame was waiting: the newest clean one
// replaces buf (the older one is requeued); corrupt or empty ones go straight
// back. A failed DQBUF ends the skip and keeps buf.
void CameraV4L2::skip_to_newest( v4l2_buffer& buf )
{
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
}

void CameraV4L2::requeue( int index ) noexcept  // NOLINT(readability-make-member-function-const)
{
    v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    int r = 0;
    do
    {
        r = ioctl( fd_, VIDIOC_QBUF, &buf );
    } while ( r < 0 && errno == EINTR );
    if ( r < 0 )
    {
        // A failed QBUF silently removes the buffer from rotation;
        // without this log the symptom is only a mysterious cam-time growth.
        const int e = errno;
        log_.warn() << "camera: QBUF failed for idx=" << index << ": " << ErrnoText { e } << '\n';
    }
}
