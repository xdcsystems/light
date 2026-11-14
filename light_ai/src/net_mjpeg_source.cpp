#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include "net_mjpeg_source.hpp"

#include "core/http_head.hpp"
#include "core/http_url.hpp"
#include "jpeg_probe.hpp"

namespace
{
    // Parser-layer status codes. kDesync is the resync protocol: a
    // wedged-full socket window (net_fill) or a stream that no longer
    // parses (MultipartReader::Event::Desync) -- the caller drops the
    // buffered window, counts the error streak and resyncs (read_latest_part
    // retries once; the fetch path maps it to EAGAIN vs EIO). -1 stays
    // numeric on purpose: in the connect/handshake code it is a generic
    // failure, while the readers propagate net_fill's "0 = EOF, -1 = error"
    // collapse -- naming it would fake a distinction the protocol does not make.
    constexpr int kDesync = -2;

    // jpeg_frame_check plus the geometry of the stream.
    bool frame_ok( const uint8_t* data, uint32_t& size, int w, int h )
    {
        JpegSofInfo info {};
        return jpeg_frame_check( data, size, info ) && info.w == w && info.h == h;
    }
}  // namespace

NetMjpegSource::NetMjpegSource()
{
    // Init-time allocation (the rknn_engine out_mems_ precedent):
    // one anonymous mmap for the whole pool — not heap, never touched again.
    // The closest analog of the V4L2 driver-owned buffers this class
    // stands in for. A failed mmap is not fatal here: setup_device
    // reports it and the pipeline fails loud, as everywhere.
    const size_t bytes = static_cast<size_t>( kBufCount ) * static_cast<size_t>( kFrameMax );
    pool_ = ::mmap( nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if ( pool_ == reinterpret_cast<void *>( -1 ) )  // MAP_FAILED: its ((void *) -1) lives in a system header
        return;  // pool_ stays nullptr; setup_device fails loud
    for ( int k = 0; k < kBufCount; ++k )
    {
        bufs_[k].start = static_cast<uint8_t*>( pool_ ) + static_cast<size_t>( k ) * static_cast<size_t>( kFrameMax );
        bufs_[k].length = static_cast<size_t>( kFrameMax );
    }
}

NetMjpegSource::~NetMjpegSource()
{
    teardown();
    if ( pool_ != nullptr )
        ::munmap( pool_, static_cast<size_t>( kBufCount ) * static_cast<size_t>( kFrameMax ) );
}

int NetMjpegSource::setup_device( const std::string& url, std::string* why )
{
    if ( pool_ == nullptr )
    {
        if ( why )
            *why = "frame pool mmap failed at construction";
        return -1;
    }
    teardown();

    HttpUrl u;
    if ( !parse_http_url( url, u, why ) )
        return -1;
    if ( connect_tcp( u.host, u.port, why ) != 0 )
        return -1;
    if ( send_get( u.host, u.port, u.path, why ) != 0 )
    {
        teardown();
        return -1;
    }
    if ( read_headers( why ) != 0 )
    {
        teardown();
        return -1;
    }
    // Prime the first JPEG frame into slot 0 to read its SOFn geometry
    // before MppDecoder / NetEncoder / RgaResizer are initialized.
    uint32_t n = 0;
    if ( read_part( 0, &n ) != 0 )
    {
        if ( why )
            *why = "failed to read initial JPEG frame";
        teardown();
        return -1;
    }
    JpegSofInfo info {};
    if ( !jpeg_frame_check( static_cast<const uint8_t*>( bufs_[0].start ), n, info ) )
    {
        if ( why )
            *why = "initial JPEG frame is incomplete or missing SOFn";
        teardown();
        return -1;
    }
    if ( width_ > 0 && height_ > 0 && ( info.w != width_ || info.h != height_ ) )
    {
        if ( why )
            *why = "stream resolution changed on reconnect";
        teardown();
        return -1;
    }
    width_ = info.w;
    height_ = info.h;
    primed_len_ = n;
    std::cout << "net-src: connected " << url << " (" << width_ << 'x' << height_ << "), boundary " << parts_.boundary() << "\n";
    err_streak_ = 0;
    return 0;
}

void NetMjpegSource::teardown()
{
    if ( fd_ >= 0 )
    {
        ::close( fd_ );
        fd_ = -1;
    }
    win_.clear();
    primed_len_ = 0;
    for ( auto& b : bufs_ )
        b.in_flight = false;
}

int NetMjpegSource::recover( const std::string& id, std::string& new_id, const std::vector<std::string>&,
                             std::string* why )
{
    new_id = id;  // a URL never re-enumerates
    return setup_device( id, why );
}

int NetMjpegSource::connect_tcp( const std::string& host, int port, std::string* why )
{
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port_str = std::to_string( port );  // named: c_str() must outlive getaddrinfo
    addrinfo* res = nullptr;
    if ( getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res ) != 0 || res == nullptr )
    {
        if ( why )
            *why = "addr resolution failed for " + host;
        return -1;
    }
    // SOCK_CLOEXEC: this fd must not leak into a fork()/exec() (this app
    // never forks, but a library or deploy wrapper might). The flag belongs
    // to the socket() type argument -- putting SOCK_CLOEXEC into
    // hints.ai_socktype makes getaddrinfo fail with EAI_SOCKTYPE, killing
    // the connection outright.
    fd_ = ::socket( res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol );
    if ( fd_ < 0 )
    {
        if ( why )
            *why = std::string( "socket: " ) + strerror( errno );
        freeaddrinfo( res );
        return -1;
    }
    // Non-blocking connect with a 5 s window, then back to blocking I/O.
    const int fl = ::fcntl( fd_, F_GETFL, 0 );
    ::fcntl( fd_, F_SETFL, fl | O_NONBLOCK );
    const int cr = ::connect( fd_, res->ai_addr, res->ai_addrlen );
    freeaddrinfo( res );
    if ( cr != 0 && errno != EINPROGRESS )
    {
        if ( why )
            *why = std::string( "connect: " ) + strerror( errno );
        teardown();
        return -1;
    }
    if ( cr != 0 )
    {
        pollfd p { fd_, POLLOUT, 0 };
        const int pr = ::poll( &p, 1, 5000 );
        int so_err = 0;
        socklen_t slen = sizeof( so_err );
        if ( pr <= 0 || ::getsockopt( fd_, SOL_SOCKET, SO_ERROR, &so_err, &slen ) != 0 || so_err != 0 )
        {
            if ( why )
                *why = pr <= 0 ? std::string( "connect: timeout" ) : std::string( "connect: " ) + strerror( so_err );
            teardown();
            return -1;
        }
    }
    ::fcntl( fd_, F_SETFL, fl );
    int nd = 1;
    ::setsockopt( fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof( nd ) );
    return 0;
}

int NetMjpegSource::send_get( const std::string& host, int port, const std::string& path, std::string* why ) const
{
    // Streamed together, no printf-family (project rule 2).
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string( port )
        + "\r\nConnection: keep-alive\r\nAccept: multipart/x-mixed-replace, image/jpeg\r\nUser-Agent: light_ai/1.0\r\n\r\n";
    size_t off = 0;
    while ( off < req.size() )
    {
        // MSG_NOSIGNAL: a mid-GET disconnect must not raise SIGPIPE;
        // the flag keeps this call self-contained even if the global
        // ignore is ever removed.
        const ssize_t w = ::send( fd_, req.data() + off, req.size() - off, MSG_NOSIGNAL );
        if ( w < 0 && errno == EINTR )
            continue;
        if ( w <= 0 )
        {
            // w == 0 on a blocking socket cannot happen for a nonzero
            // buffer, but errno is only set on failure: reading it
            // after a zero return would report a stale value from an
            // unrelated earlier call.
            if ( why )
                *why = std::string( "send: " )
                       + ( w < 0 ? strerror( errno ) : "peer closed, 0 bytes sent" );
            return -1;
        }
        off += static_cast<size_t>( w );
    }
    return 0;
}

int NetMjpegSource::net_fill( int timeout_ms )
{
    if ( fd_ < 0 )
        return -1;
    uint8_t* const dst = win_.tail();  // compacts the unconsumed bytes to the front
    if ( win_.room() == 0 )
        return kDesync;  // wedged full: the caller's desync protocol
    pollfd p { fd_, POLLIN, 0 };
    const int pr = ::poll( &p, 1, timeout_ms >= 0 ? timeout_ms : 0 );
    if ( pr < 0 )
        return errno == EINTR ? kIntr : -1;
    if ( pr == 0 )
        return 0;
    const ssize_t n = ::recv( fd_, dst, win_.room(), 0 );
    if ( n < 0 && errno == EINTR )
        return kIntr;  // interrupted before any data (e.g. a stop signal arriving
                       // between poll() and recv()): not a connection error and
                       // not a watchdog expiry -- callers retry the fill
    if ( n <= 0 )
        return -1;  // 0 = EOF, -1 = error: the connection is gone either way
    win_.commit( static_cast<size_t>( n ) );
    return static_cast<int>( n );
}

int NetMjpegSource::read_headers( std::string* why )
{
    // parse_http_head owns the grammar and the kHeadMax cap (a server that
    // drips headers forever is a data-volume cap, not a wall-clock one).
    while ( true )
    {
        const HttpHead h = parse_http_head( win_.data(), win_.size() );
        if ( h.status == HttpHead::Status::Ok )
        {
            parts_.reset( h.boundary, h.boundary_len );  // fits: both caps are 60
            win_.consume( h.len );
            return 0;
        }
        if ( h.status == HttpHead::Status::Error )
        {
            if ( why )
                *why = h.why;
            return -1;
        }
        const int r = net_fill( kPollMs );
        if ( r == kIntr )
            continue;  // EINTR: transient, not a watchdog expiry
        if ( r <= 0 )
        {
            if ( why )
                *why = "no HTTP response within the watchdog window";
            return -1;
        }
    }
}

bool NetMjpegSource::part_buffered() const
{
    // A wedged-full window counts as ready: the read that follows desyncs.
    return win_.full() || parts_.part_buffered( win_.data(), win_.size() );
}

void NetMjpegSource::drop_window()
{
    win_.clear();
    parts_.resync();
}

int NetMjpegSource::poll_part_ready( int timeout_ms )
{
    if ( part_buffered() )
        return 1;
    int wait_ms = timeout_ms;
    while ( true )
    {
        const int r = net_fill( wait_ms );
        if ( r == kIntr )
            return 0;  // EINTR: not ready yet and not a timeout -- caller retries
        if ( r < 0 )
            return r;
        if ( part_buffered() )
            return 1;
        if ( r == 0 )
            return 0;
        wait_ms = 0;
    }
}

bool NetMjpegSource::wait_part_ready( int timeout_ms, int* err )
{
    if ( timeout_ms < 0 )
        return true;
    const int pr = poll_part_ready( timeout_ms );
    if ( pr > 0 )
        return true;
    if ( pr == kDesync )
    {
        ++err_streak_;
        drop_window();
    }
    if ( err )
        *err = ( pr == 0 || ( pr == kDesync && err_streak_ < kErrStreakMax ) ) ? EAGAIN : EIO;
    return false;
}

void NetMjpegSource::start()
{
    // Drain frames accumulated in the TCP socket while the Pipeline
    // constructor initialized NPU tensors, encoder and GPU drawer,
    // keeping only the freshest valid frame in slot 0 (primed_len_).
    // Read into slot 1 first so a failed/corrupt drain never clobbers
    // the valid primed frame already sitting in slot 0.
    for ( int guard = 0; guard < 64 && poll_part_ready( 0 ) > 0; ++guard )
    {
        uint32_t n = 0;
        if ( read_part( 1, &n ) != 0 )
            break;
        if ( frame_ok( static_cast<const uint8_t*>( bufs_[1].start ), n, width_, height_ ) )
        {
            std::swap( bufs_[0].start, bufs_[1].start );
            primed_len_ = n;
        }
    }
}

int NetMjpegSource::read_part( int slot, uint32_t* size )
{
    // The reader parses what the window holds; the window is refilled
    // until one part is complete. Any abandoned read resyncs the reader (the
    // next read starts at a delimiter line, as before); only a desync drops
    // the window, in the callers.
    auto* const dst = static_cast<uint8_t*>( bufs_[slot].start );
    while ( true )
    {
        const MultipartReader::Step st = parts_.feed( win_.data(), win_.size(), dst );
        win_.consume( st.consumed );
        if ( st.ev == MultipartReader::Event::Part )
        {
            *size = st.size;
            return 0;
        }
        if ( st.ev == MultipartReader::Event::Desync )
        {
            parts_.resync();
            return kDesync;
        }
        const int r = net_fill( kPollMs );
        if ( r == kIntr )
            continue;  // EINTR: transient, not a watchdog expiry
        if ( r <= 0 )  // 0 = watchdog, -1 = EOF / error, kDesync = wedged-full window
        {
            parts_.resync();
            return r == kDesync ? kDesync : -1;
        }
    }
}

int NetMjpegSource::read_latest_part( int slot, uint32_t* size )
{
    uint32_t n = 0;
    int rc = read_part( slot, &n );
    if ( rc == kDesync )
    {
        // Desynced stream: drop the buffered window, count the streak,
        // one resync retry before handing over to the reconnect ladder.
        ++err_streak_;
        drop_window();
        if ( err_streak_ < kErrStreakMax )
        {
            n = 0;
            rc = read_part( slot, &n );
        }
    }
    if ( rc != 0 )
        return rc;
    int scratch = -1;
    for ( int k = 0; k < kBufCount; ++k )
    {
        if ( k != slot && !bufs_[k].in_flight )
        {
            scratch = k;
            break;
        }
    }
    // If multiple frames queued up in the TCP buffer during a Wi-Fi burst,
    // advance to the freshest complete frame via a scratch slot so a failed
    // or corrupt skip never clobbers the frame already in `slot`.
    for ( int skip = 0; scratch >= 0 && skip < 4 && poll_part_ready( 0 ) > 0; ++skip )
    {
        uint32_t next_n = 0;
        if ( const int sr = read_part( scratch, &next_n ); sr != 0 )
        {
            if ( sr == kDesync )
                drop_window();
            break;
        }
        if ( frame_ok( static_cast<const uint8_t*>( bufs_[scratch].start ), next_n, width_, height_ ) )
        {
            std::swap( bufs_[slot].start, bufs_[scratch].start );
            n = next_n;
        }
    }
    *size = n;
    return 0;
}

int NetMjpegSource::dequeue( uint32_t* size, int* err, int timeout_ms )
{
    if ( err )
        *err = 0;
    if ( fd_ < 0 )
    {
        if ( err )
            *err = ENODEV;
        return -1;
    }
    if ( err_streak_ >= kErrStreakMax )
    {
        if ( err )
            *err = EIO;  // recoverable stall -> the reconnect ladder
        return -1;
    }
    if ( primed_len_ > 0 )
    {
        bufs_[0].in_flight = true;
        if ( size )
            *size = primed_len_;
        primed_len_ = 0;
        err_streak_ = 0;
        return 0;
    }
    if ( !wait_part_ready( timeout_ms, err ) )
        return -1;
    int slot = -1;
    for ( int k = 0; k < kBufCount; ++k )
    {
        if ( !bufs_[k].in_flight )
        {
            slot = k;
            break;
        }
    }
    if ( slot < 0 )
    {
        // Contract: requeue() precedes the next dequeue(); a full pool
        // here is a caller bug — loud, then slot 0 defensively.
        std::cerr << "net-src: all buffers in flight, reusing slot 0 (pipeline contract violation)\n";
        slot = 0;
    }
    uint32_t n = 0;
    if ( const int rc = read_latest_part( slot, &n ); rc != 0 )
    {
        if ( err )
            *err = EIO;  // EOF / watchdog / recv error: recoverable
        return -1;
    }
    if ( !frame_ok( static_cast<const uint8_t*>( bufs_[slot].start ), n, width_, height_ ) )
    {
        ++err_streak_;
        std::cerr << "net-src: corrupt/incomplete JPEG frame (" << n
                  << " bytes, streak " << err_streak_ << ") dropped\n";
        if ( err )
            *err = ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN;
        return -1;
    }
    err_streak_ = 0;
    bufs_[slot].in_flight = true;
    if ( size )
        *size = n;
    return slot;
}

void NetMjpegSource::requeue( int index )
{
    if ( index >= 0 && index < kBufCount )
        bufs_[index].in_flight = false;
}

