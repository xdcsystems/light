#include <cctype>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdlib>
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

#include "jpeg_probe.hpp"

namespace
{
    constexpr size_t kNpos = static_cast<size_t>( -1 );

    bool parse_http_url( const std::string& url, std::string& host, int& port, std::string& path, std::string* why )
    {
        // No named constant here on purpose: a function-local static
        // constexpr needs the s_ prefix (naming), a plain constexpr needs
        // lower_case (naming), and dropping static trips
        // static-definition-in-anonymous-namespace. No named form passes
        // all three; magic-number checks are disabled project-wide.
        if ( url.compare( 0, 7, "http://" ) != 0 )
        {
            if ( why )
                *why = "url must start with http://";
            return false;
        }
        const std::string body = url.substr( 7 );
        const size_t slash = body.find( '/' );
        const std::string hp = ( slash == std::string::npos ) ? body : body.substr( 0, slash );
        path = ( slash == std::string::npos ) ? std::string( "/" ) : body.substr( slash );
        const size_t colon = hp.rfind( ':' );
        if ( colon == std::string::npos )
        {
            host = hp;
            port = 80;
        }
        else
        {
            host = hp.substr( 0, colon );
            // strtol with a full parse check (cert-err34-c): no atoi.
            const char* ps = hp.c_str() + colon + 1;
            errno = 0;
            char* end = nullptr;
            const long pv = std::strtol( ps, &end, 10 );
            if ( errno != 0 || end == ps || *end != '\0' || pv < 1 || pv > 65535 )
            {
                if ( why )
                    *why = "bad port in url";
                return false;
            }
            port = static_cast<int>( pv );
        }
        if ( host.empty() )
        {
            if ( why )
                *why = "empty host in url";
            return false;
        }
        return true;
    }

    // Case-insensitive search in an ASCII buffer; kNpos when absent.
    size_t ci_find( const uint8_t* hay, size_t hay_len, const char* needle )
    {
        const size_t nlen = std::strlen( needle );
        for ( size_t i = 0; i + nlen <= hay_len; ++i )
        {
            size_t k = 0;
            while ( k < nlen
                    && std::tolower( static_cast<unsigned char>( hay[i + k] ) )
                           == std::tolower( static_cast<unsigned char>( needle[k] ) ) )
                ++k;
            if ( k == nlen )
                return i;
        }
        return kNpos;
    }

    // Structural validation via probe_jpeg_sof (jpeg_probe.hpp):
    // SOI -> markers -> SOF (dims/sampling) -> SOS, plus a trailing EOI.
    // The contract is unchanged: *inout_size is trimmed of trailing
    // HTTP CRLF/space IN PLACE (both call sites rely on the trimmed
    // length), and any structural doubt returns false (the corrupt streak).
    bool jpeg_frame_complete( const uint8_t* data, uint32_t* inout_size, int* out_w, int* out_h )
    {
        uint32_t sz = *inout_size;
        while ( sz > 2 && ( data[sz - 1] == '\r' || data[sz - 1] == '\n' || data[sz - 1] == ' ' ) )
            --sz;
        *inout_size = sz;
        if ( sz < 4 || data[0] != 0xFF || data[1] != 0xD8 )
            return false;
        JpegSofInfo info {};
        if ( !probe_jpeg_sof( data, sz, info ) )
            return false;
        if ( out_w )
            *out_w = info.w;
        if ( out_h )
            *out_h = info.h;
        return data[sz - 2] == 0xFF && data[sz - 1] == 0xD9;
    }
    // In-bounds Content-Length parser shared by read_part_header() and
    // has_buffered_header(): replaces the copy-pasted ci_find + strtol
    // blocks, which scanned for a terminator that may lie outside the
    // non-NUL-terminated window (read past sock_ when the value touched
    // its edge) and silently accepted partial values cut by the buffer.
    // Strict contract: no signs, no embedded whitespace, digits must end
    // at an in-bounds CR/LF; overflow is rejected.
    bool parse_content_length( const uint8_t* ph, size_t phl, long* out )
    {
        const size_t clp = ci_find( ph, phl, "content-length:" );
        if ( clp == kNpos )
            return false;
        size_t p = clp + 15;  // past "content-length:"
        while ( p < phl && ( ph[p] == ' ' || ph[p] == '\t' ) )
            ++p;
        if ( p >= phl || ph[p] < '0' || ph[p] > '9' )
            return false;
        long v = 0;
        while ( p < phl && ph[p] >= '0' && ph[p] <= '9' )
        {
            if ( v > ( LONG_MAX - ( ph[p] - '0' ) ) / 10 )
                return false;  // overflow: not a sane Content-Length
            v = v * 10 + ( ph[p] - '0' );
            ++p;
        }
        if ( p >= phl || ( ph[p] != '\r' && ph[p] != '\n' ) )
            return false;  // truncated mid-value or trailing junk
        *out = v;
        return true;
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
    sock_pos_ = sock_end_ = 0;
    boundary_len_ = 0;
    bpat_len_ = 0;

    std::string host;
    std::string path;
    int port = 0;
    if ( !parse_http_url( url, host, port, path, why ) )
        return -1;
    if ( connect_tcp( host, port, why ) != 0 )
        return -1;
    if ( send_get( host, port, path, why ) != 0 )
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
    int fw = 0;
    int fh = 0;
    const auto* p = static_cast<const uint8_t*>( bufs_[0].start );
    if ( !jpeg_frame_complete( p, &n, &fw, &fh ) )
    {
        if ( why )
            *why = "initial JPEG frame is incomplete or missing SOFn";
        teardown();
        return -1;
    }
    if ( width_ > 0 && height_ > 0 && ( fw != width_ || fh != height_ ) )
    {
        if ( why )
            *why = "stream resolution changed on reconnect";
        teardown();
        return -1;
    }
    width_ = fw;
    height_ = fh;
    primed_len_ = n;
    std::cout << "net-src: connected " << url << " (" << width_ << 'x' << height_ << "), boundary " << boundary_ << "\n";
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
    sock_pos_ = sock_end_ = 0;
    primed_len_ = 0;
    for ( auto& b : bufs_ )
        b.in_flight = false;
}

int NetMjpegSource::recover( const std::string& id, std::string& new_id, std::string* why )
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
    fd_ = ::socket( res->ai_family, res->ai_socktype, res->ai_protocol );
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
    if ( sock_pos_ > 0 )
    {
        std::memmove( sock_, sock_ + sock_pos_, sock_end_ - sock_pos_ );
        sock_end_ -= sock_pos_;
        sock_pos_ = 0;
    }
    if ( sock_end_ == static_cast<size_t>( kSockBuf ) )
        return -2;  // wedged full: the caller's desync protocol
    pollfd p { fd_, POLLIN, 0 };
    const int pr = ::poll( &p, 1, timeout_ms >= 0 ? timeout_ms : 0 );
    if ( pr < 0 )
        return errno == EINTR ? 0 : -1;
    if ( pr == 0 )
        return 0;
    const ssize_t n = ::recv( fd_, sock_ + sock_end_, static_cast<size_t>( kSockBuf ) - sock_end_, 0 );
    if ( n < 0 && errno == EINTR )
        return 0;  // interrupted before any data (e.g. a stop signal arriving
                   // between poll() and recv()): not a connection error, the
                   // caller retries on the next fill -- treating it as fatal
                   // would turn Ctrl-C shutdown into a spurious reconnect
    if ( n <= 0 )
        return -1;  // 0 = EOF, -1 = error: the connection is gone either way
    sock_end_ += static_cast<size_t>( n );
    return static_cast<int>( n );
}

size_t NetMjpegSource::net_find( const char* pat, size_t plen ) const
{
    const size_t avail = sock_end_ - sock_pos_;
    for ( size_t p = 0; p + plen <= avail; ++p )
    {
        if ( std::memcmp( sock_ + sock_pos_ + p, pat, plen ) == 0 )
            return p;
    }
    return kNpos;
}

int NetMjpegSource::read_headers( std::string* why )
{
    // Bounded by kHdrMax: a server that drips headers forever is a
    // data-volume cap, not a wall-clock one.
    while ( net_find( "\r\n\r\n", 4 ) == kNpos )
    {
        if ( sock_end_ - sock_pos_ >= static_cast<size_t>( kHdrMax ) )
        {
            if ( why )
                *why = "response headers oversized";
            return -1;
        }
        if ( net_fill( kPollMs ) <= 0 )
        {
            if ( why )
                *why = "no HTTP response within the watchdog window";
            return -1;
        }
    }
    const size_t hdr_end = net_find( "\r\n\r\n", 4 ) + sock_pos_ + 4;  // guaranteed by the loop
    const size_t hl = hdr_end - 4 - sock_pos_;
    const uint8_t* h = sock_ + sock_pos_;

    // Status line: bounded by the first CRLF; require HTTP 200.
    size_t line_end = 0;
    while ( line_end + 1 < hl && !( h[line_end] == '\r' && h[line_end + 1] == '\n' ) )
        ++line_end;
    if ( ci_find( h, line_end, " 200 " ) == kNpos )
    {
        if ( why )
            *why = "server did not answer 200";
        return -1;
    }

    // The boundary is mandatory: without it there is no framing at all.
    const size_t bpos = ci_find( h, hl, "boundary=" );
    if ( bpos == kNpos )
    {
        if ( why )
            *why = "no multipart boundary in Content-Type";
        return -1;
    }
    size_t v = bpos + 9;
    size_t vlen = 0;
    while ( v + vlen < hl && h[v + vlen] != '\r' && h[v + vlen] != '\n' && h[v + vlen] != '"'
            && h[v + vlen] != ';' && vlen < static_cast<size_t>( kBoundaryMax ) )
        ++vlen;
    // Fail loud on an over-long boundary instead of a silent snprintf
    // truncation: a truncated pattern would desync the whole stream.
    if ( vlen == static_cast<size_t>( kBoundaryMax ) && v + vlen < hl && h[v + vlen] != '\r' && h[v + vlen] != '\n'
         && h[v + vlen] != '"' && h[v + vlen] != ';' )
    {
        if ( why )
            *why = "multipart boundary too long";
        return -1;
    }
    if ( vlen == 0 )
    {
        if ( why )
            *why = "empty multipart boundary";
        return -1;
    }
    std::memcpy( boundary_, h + v, vlen );
    boundary_[vlen] = '\0';
    boundary_len_ = static_cast<int>( vlen );
    // "\r\n--" + boundary + "\r\n", built with plain copies (rule 2).
    // The kBoundaryMax cap above guarantees the fit: 4 + 60 + 2 = 66.
    std::memcpy( bpat_, "\r\n--", 4 );
    std::memcpy( bpat_ + 4, boundary_, vlen );
    std::memcpy( bpat_ + 4 + vlen, "\r\n", 2 );
    bpat_len_ = static_cast<int>( 4 + vlen + 2 );

    sock_pos_ = hdr_end;
    return 0;
}

int NetMjpegSource::read_boundary()
{
    // Shape B (IP Webcam style): the first part may follow the response
    // headers without a leading CRLF — accept "--boundary\r\n" at the
    // window start.
    if ( bpat_len_ < 6 )
        return -1;
    const size_t bare_len = static_cast<size_t>( bpat_len_ ) - 2;
    while ( true )
    {
        const size_t avail = sock_end_ - sock_pos_;
        if ( avail >= bare_len && std::memcmp( sock_ + sock_pos_, bpat_ + 2, bare_len ) == 0 )
        {
            sock_pos_ += bare_len;
            return 0;
        }
        const size_t f = net_find( bpat_, static_cast<size_t>( bpat_len_ ) );
        if ( f != kNpos )
        {
            sock_pos_ += f + static_cast<size_t>( bpat_len_ );
            return 0;
        }
        if ( avail >= static_cast<size_t>( kSockBuf ) )
            return -2;  // 256 KiB without a boundary: desynced stream
        const int r = net_fill( kPollMs );
        if ( r == -2 )
            return -2;
        if ( r <= 0 )
            return -1;
    }
}

int NetMjpegSource::read_part_header( long* need )
{
    while ( net_find( "\r\n\r\n", 4 ) == kNpos )
    {
        if ( sock_end_ - sock_pos_ >= static_cast<size_t>( kSockBuf ) )
            return -2;
        if ( net_fill( kPollMs ) <= 0 )
            return -1;
    }
    const size_t hend = net_find( "\r\n\r\n", 4 ) + sock_pos_ + 4;  // guaranteed by the loop
    const uint8_t* ph = sock_ + sock_pos_;
    const size_t phl = hend - 4 - sock_pos_;
    *need = -1;
    parse_content_length( ph, phl, need );
    sock_pos_ = hend;
    return 0;
}

int NetMjpegSource::receive_body( int slot, long need, uint32_t* size )
{
    auto* dst = static_cast<uint8_t*>( bufs_[slot].start );
    if ( need < 0 )
    {
        // No Content-Length: copy the body to the slot while scanning
        // for the closing boundary — keep the last (bpat_len_ - 1) bytes
        // in sock_ on each refill so a boundary split across two recv()
        // calls is never cut in half, and leave bpat_ unconsumed for the
        // next frame's read_boundary().
        const size_t pat_len = static_cast<size_t>( bpat_len_ );
        const size_t keep = pat_len > 1 ? pat_len - 1 : 0;
        size_t got = 0;
        while ( true )
        {
            const size_t f = net_find( bpat_, pat_len );
            if ( f != kNpos )
            {
                if ( got + f > static_cast<size_t>( kFrameMax ) )
                    return -2;
                std::memcpy( dst + got, sock_ + sock_pos_, f );
                sock_pos_ += f;
                *size = static_cast<uint32_t>( got + f );
                return 0;
            }
            const size_t avail = sock_end_ - sock_pos_;
            if ( avail > keep )
            {
                const size_t flush = avail - keep;
                if ( got + flush > static_cast<size_t>( kFrameMax ) )
                    return -2;
                std::memcpy( dst + got, sock_ + sock_pos_, flush );
                sock_pos_ += flush;
                got += flush;
            }
            if ( net_fill( kPollMs ) <= 0 )
                return -1;
        }
    }
    if ( static_cast<size_t>( need ) > static_cast<size_t>( kFrameMax ) )
        return -2;  // oversized "part": desynced garbage, resync
    // Copy the frame body in chunks: the socket window is refilled as
    // it drains, the watchdog covers the whole frame, not one chunk.
    size_t got = 0;
    while ( got < static_cast<size_t>( need ) )
    {
        if ( sock_pos_ >= sock_end_ )
        {
            if ( net_fill( kPollMs ) <= 0 )
                return -1;
            continue;
        }
        const size_t avail = sock_end_ - sock_pos_;
        const size_t want = static_cast<size_t>( need ) - got;
        const size_t take = avail < want ? avail : want;
        std::memcpy( dst + got, sock_ + sock_pos_, take );
        sock_pos_ += take;
        got += take;
    }
    *size = static_cast<uint32_t>( need );
    return 0;
}

bool NetMjpegSource::has_buffered_header() const
{
    if ( bpat_len_ < 6 )
        return false;
    const size_t avail = sock_end_ - sock_pos_;
    if ( avail >= static_cast<size_t>( kSockBuf ) )
        return true;
    const size_t bare_len = static_cast<size_t>( bpat_len_ ) - 2;
    size_t b_end = kNpos;
    if ( avail >= bare_len && std::memcmp( sock_ + sock_pos_, bpat_ + 2, bare_len ) == 0 )
    {
        b_end = bare_len;
    }
    else
    {
        const size_t f = net_find( bpat_, static_cast<size_t>( bpat_len_ ) );
        if ( f != kNpos )
            b_end = f + static_cast<size_t>( bpat_len_ );
    }
    if ( b_end == kNpos || avail < b_end + 4 )
        return false;
    size_t h_end = kNpos;
    for ( size_t p = b_end; p + 4 <= avail; ++p )
    {
        if ( std::memcmp( sock_ + sock_pos_ + p, "\r\n\r\n", 4 ) == 0 )
        {
            h_end = p + 4;
            break;
        }
    }
    if ( h_end == kNpos )
        return false;
    const uint8_t* ph = sock_ + sock_pos_ + b_end;
    const size_t phl = h_end - 4 - b_end;
    long cl = 0;
    if ( parse_content_length( ph, phl, &cl ) )
    {
        if ( cl > kFrameMax )
            return true;
        return ( avail - h_end ) >= static_cast<size_t>( cl );
    }
    const size_t pat_len = static_cast<size_t>( bpat_len_ );
    for ( size_t p = h_end; p + pat_len <= avail; ++p )
    {
        if ( std::memcmp( sock_ + sock_pos_ + p, bpat_, pat_len ) == 0 )
            return true;
    }
    return false;
}

int NetMjpegSource::poll_part_ready( int timeout_ms )
{
    if ( has_buffered_header() )
        return 1;
    int wait_ms = timeout_ms;
    while ( true )
    {
        const int r = net_fill( wait_ms );
        if ( r < 0 )
            return r;
        if ( has_buffered_header() )
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
    if ( pr == -2 )
    {
        ++err_streak_;
        sock_pos_ = sock_end_;
    }
    if ( err )
        *err = ( pr == 0 || ( pr == -2 && err_streak_ < kErrStreakMax ) ) ? EAGAIN : EIO;
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
        int fw = 0;
        int fh = 0;
        const auto* p = static_cast<const uint8_t*>( bufs_[1].start );
        if ( jpeg_frame_complete( p, &n, &fw, &fh ) && fw == width_ && fh == height_ )
        {
            std::swap( bufs_[0].start, bufs_[1].start );
            primed_len_ = n;
        }
    }
}

int NetMjpegSource::read_part( int slot, uint32_t* size )
{
    long need = 0;
    int rc = read_boundary();
    if ( rc != 0 )
        return rc;
    rc = read_part_header( &need );
    if ( rc != 0 )
        return rc;
    return receive_body( slot, need, size );
}

int NetMjpegSource::read_latest_part( int slot, uint32_t* size )
{
    uint32_t n = 0;
    int rc = read_part( slot, &n );
    if ( rc == -2 )
    {
        // Desynced stream: drop the buffered window, count the streak,
        // one resync retry before handing over to the reconnect ladder.
        ++err_streak_;
        sock_pos_ = sock_end_;
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
        const int sr = read_part( scratch, &next_n );
        if ( sr != 0 )
        {
            if ( sr == -2 )
                sock_pos_ = sock_end_;
            break;
        }
        int fw = 0;
        int fh = 0;
        const auto* p = static_cast<const uint8_t*>( bufs_[scratch].start );
        if ( jpeg_frame_complete( p, &next_n, &fw, &fh ) && fw == width_ && fh == height_ )
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
    const int rc = read_latest_part( slot, &n );
    if ( rc != 0 )
    {
        if ( err )
            *err = EIO;  // EOF / watchdog / recv error: recoverable
        return -1;
    }
    const uint8_t* p = static_cast<const uint8_t*>( bufs_[slot].start );
    int fw = 0;
    int fh = 0;
    if ( !jpeg_frame_complete( p, &n, &fw, &fh ) || fw != width_ || fh != height_ )
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

