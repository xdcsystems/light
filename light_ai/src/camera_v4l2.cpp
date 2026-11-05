#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <map>

#include "camera_v4l2.hpp"

// True when the node is a V4L2 capture device sitting on a USB bus
// (QUERYCAP bus_info contains "usb"). Everything else on this board —
// rkcif/rkisp platform nodes — is rejected before any S_FMT is tried:
// they would burn reconnect attempts on setup failures, and a platform
// capture node might even accept the format while never delivering a
// frame (no sensor attached) — a false "successful" reconnect.
namespace
{
    bool usb_capture_node( const std::string& path )
    {
        const int fd = ::open( path.c_str(), O_RDONLY | O_CLOEXEC );
        if ( fd < 0 )
            return false;
        v4l2_capability cap {};
        while ( ::ioctl( fd, VIDIOC_QUERYCAP, &cap ) < 0 && errno == EINTR )
        {}
        bool ok = false;
        // CAP_STREAMING (P1-5): a node that only supports read()/write()
        // I/O passes the CAPTURE check but dies at QUERYBUF with a
        // confusing error - reject it here, where the diagnosis is clear.
        if ( ( cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ) &&
             ( cap.capabilities & V4L2_CAP_STREAMING ) )
        {
            const char* bus = reinterpret_cast<const char*>( cap.bus_info );
            ok = bus != nullptr && std::strstr( bus, "usb" ) != nullptr;
        }
        ::close( fd );
        return ok;
    }
}  // namespace

CameraV4L2::CameraV4L2( int w, int h ) : width_( w ), height_( h )
{}

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
        std::cerr << "open camera: " << strerror( errno ) << '\n';
        if ( why )
            *why = "open failed";
        return -1;
    }

    v4l2_format fmt {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width_;
    fmt.fmt.pix.height = height_;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if ( xioctl_r( VIDIOC_S_FMT, &fmt ) < 0 )
    {
        std::cerr << "camera: S_FMT: " << strerror( errno ) << '\n';
        if ( why )
            *why = "S_FMT failed";
        teardown();
        return -1;
    }

    // The driver may silently substitute a closest supported mode.
    // Verify what we actually got and fail loudly on a mismatch,
    // otherwise the whole pipeline (RGA, MPP) would run with wrong dims.
    v4l2_format gfmt {};
    gfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if ( xioctl_r( VIDIOC_G_FMT, &gfmt ) < 0 )
    {
        std::cerr << "camera: G_FMT: " << strerror( errno ) << '\n';
        if ( why )
            *why = "G_FMT failed";
        teardown();
        return -1;
    }
    if ( gfmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG || 
         gfmt.fmt.pix.width != static_cast<unsigned>( width_ ) ||
         gfmt.fmt.pix.height != static_cast<unsigned>( height_ ) )
    {
        std::cerr << "camera: mode mismatch: requested " << width_ << 'x' << height_ << " MJPEG, got " << gfmt.fmt.pix.width << 'x'
                  << gfmt.fmt.pix.height << " fourcc=" << std::hex << gfmt.fmt.pix.pixelformat << std::dec << '\n';
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
            std::cerr << "camera: requested 30 fps, driver set " << d << "\n";
    }

    v4l2_requestbuffers req {};
    req.count = kBufCount;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if ( xioctl_r( VIDIOC_REQBUFS, &req ) < 0 )
    {
        std::cerr << "camera: REQBUFS: " << strerror( errno ) << '\n';
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
    // The driver may also return FEWER buffers than requested. With < 3
    // the pipeline loses its ping-pong depth (latency and jitter grow) -
    // say so instead of silently degrading (P1-5).
    if ( req.count < 3 )
        std::cerr << "camera: REQBUFS returned only " << req.count
                  << " buffers - expect extra latency\n";

    for ( unsigned i = 0; i < req.count; ++i )
    {
        v4l2_buffer buf {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if ( xioctl_r( VIDIOC_QUERYBUF, &buf ) < 0 )
        {
            std::cerr << "camera: QUERYBUF idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "QUERYBUF failed";
            teardown();
            return -1;
        }
        bufs_[i].length = buf.length;
        bufs_[i].start = mmap( nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset );
        if ( bufs_[i].start == MAP_FAILED )
        {
            bufs_[i].start = nullptr;
            std::cerr << "camera: mmap idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "mmap failed";
            teardown();
            return -1;
        }
        if ( xioctl_r( VIDIOC_QBUF, &buf ) < 0 )
        {
            std::cerr << "camera: QBUF idx=" << i << ": " << strerror( errno ) << '\n';
            if ( why )
                *why = "QBUF failed";
            teardown();
            return -1;
        }
    }
    return 0;
}

// Release the fd and all mappings. Safe on a partially-open device and
// on an already-dead one: STREAMOFF errors are ignored on purpose (the
// fd may not exist anymore), unmapped slots are skipped via the
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
    // Do not stop at the first empty slot (P1-5): slot indices are dense
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

// Disconnect recovery: full teardown + the same setup path as startup,
// plus STREAMON. The node is supplied by the owner (see
// find_usb_capture_node) — the class never decides which node to open.
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
        std::cerr << "camera: reconnect STREAMON: " << strerror( errno ) << '\n';
        teardown();
        return -1;
    }
    err_streak_ = 0;  // P1-7: fresh device session (stream running) — the corrupt streak starts over
    return 0;
}

// Node discovery. Preferred: the configured path, if it exists and its
// full setup passes; otherwise the numeric /dev/video* candidates are
// filtered by the USB capture check and the first one whose setup passes
// wins. The trial leaves the camera closed on every outcome — the
// caller re-opens the winning node via reconnect().
int find_usb_capture_node( CameraV4L2& cam, const std::string& preferred, std::string& out )
{
    struct stat st
    {};
    std::string why;
    if ( !preferred.empty() && stat( preferred.c_str(), &st ) == 0 )
    {
        if ( cam.setup_device( preferred, &why ) == 0 )
        {
            cam.teardown();
            out = preferred;
            return 0;
        }
        std::cerr << "camera: reconnect setup on " << preferred << " failed: " << why << '\n';
        cam.teardown();
    }
    std::cerr << "camera: node " << preferred << " not present, scanning /dev/video*\n";
    std::map<int, std::string> cands;
    DIR* d = opendir( "/dev" );
    if ( d )
    {
        struct dirent* e = nullptr;
        while ( ( e = readdir( d ) ) != nullptr )
        {
            const std::string n = e->d_name;
            if ( n.rfind( "video", 0 ) != 0 || n.size() == 5 )
                continue;
            bool digits = true;
            for ( size_t i = 5; i < n.size() && digits; ++i )
                digits = isdigit( static_cast<unsigned char>( n[i] ) ) != 0;
            if ( !digits )
                continue;
            const std::string p = "/dev/" + n;
            struct stat cst {};
            if ( stat( p.c_str(), &cst ) == 0 && usb_capture_node( p ) )
                cands[static_cast<int>( std::strtol( n.c_str() + 5, nullptr, 10 ) )] = p;
        }
        closedir( d );
    }
    for ( const auto& kv : cands )
    {
        std::cerr << "camera: trying " << kv.second << '\n';
        if ( cam.setup_device( kv.second, &why ) == 0 )
        {
            cam.teardown();
            out = kv.second;
            return 0;
        }
        cam.teardown();
    }
    return -1;
}

CameraV4L2::~CameraV4L2()
{
    teardown();
}

void CameraV4L2::xioctl( unsigned long req, void* arg ) const
{
    int r = 0;

    do
    {
        r = ioctl( fd_, req, arg );
    }
    while ( r < 0 && errno == EINTR );

    if ( r < 0 )
    {
        throw std::runtime_error( std::string( "ioctl failed: " ) + strerror( errno ) );
    }
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

void CameraV4L2::start() const
{
    v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl( VIDIOC_STREAMON, &t );
}

// poll(2) with EINTR retries, in the spirit of xioctl() above:
// returns >0 (ready), 0 (timeout) or <0 (error, errno set).
static int poll_ready( int fd, int timeout_ms )
{
    pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int pr = ::poll( &pfd, 1, timeout_ms );
    while ( pr < 0 && errno == EINTR )
        pr = ::poll( &pfd, 1, timeout_ms );
    return pr;
}

int CameraV4L2::dequeue( uint32_t* size, int* err )  // NOLINT(readability-make-member-function-const)
{
    v4l2_buffer buf {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    bool have = false;  // loop until a clean buffer comes out
    while ( !have )
    {
        // Watchdog: the wait is bounded by poll(). A camera that silently
        // stops delivering frames (wedged UVC endpoint, firmware stall)
        // never produces an errno, so an unguarded blocking DQBUF hung
        // the pipeline thread forever and recovery never ran. The
        // timeout is reported as EIO, the recoverable class, so a
        // stall goes through the same reconnect path as a disappearing
        // device. 2 s is far beyond any legitimate inter-frame gap
        // (worst measured: ~53 ms).
        const int pr = poll_ready( fd_, static_cast<int>( kDequeuePollMs ) );
        if ( pr < 0 )
        {
            if ( err )
                *err = errno;
            return -1;
        }
        if ( pr == 0 )
        {
            std::cerr << "cam: no frame in " << kDequeuePollMs << " ms - treating as recoverable stall\n";
            if ( err )
                *err = EIO;
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
            // P1-7: member, not a local — the streak survives dequeue()
            // calls (e.g. corrupt frames followed by a poll timeout), so
            // the watchdog threshold reflects the real buffer history.
            ++err_streak_;
            if ( err_streak_ < static_cast<int>( kBufCount ) )
                continue;
            std::cerr << "cam: " << err_streak_ << " consecutive corrupt buffers - stall\n";
            if ( err )
                *err = EIO;
            return -1;
        }
        have = true;
    }
    err_streak_ = 0;  // P1-7: a clean buffer resets the streak
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
        std::cerr << "camera: QBUF failed for idx=" << index << ": " << strerror( errno ) << '\n';
    }
}


int CameraV4L2::recover( const std::string& id, std::string& new_id, std::string* why )
{
    // One ladder step = node discovery (a node may have re-enumerated)
    // + the full reconnect path. A failed discovery is a failed attempt.
    if ( find_usb_capture_node( *this, id, new_id ) != 0 )
    {
        if ( why )
            *why = "no usable USB capture node found";
        return -1;
    }
    return reconnect( new_id, why );
}
