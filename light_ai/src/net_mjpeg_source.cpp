#include <cerrno>
#include <string>

#include "net_mjpeg_source.hpp"

#include "core/http_head.hpp"
#include "core/http_url.hpp"
#include "core/log.hpp"
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

    // read_headers hands the head's boundary to the reader: one cap for both.
    static_assert( static_cast<int>( HttpHead::kBoundaryMax ) == static_cast<int>( MultipartReader::kBoundaryMax ),
                   "the boundary caps must match" );

    // jpeg_frame_check plus the geometry of the stream.
    bool frame_ok( const uint8_t* data, uint32_t& size, int w, int h )
    {
        JpegSofInfo info {};
        return jpeg_frame_check( data, size, info ) && info.w == w && info.h == h;
    }
}  // namespace

int NetMjpegSource::setup_device( const std::string& url, std::string* why )
{
    teardown();

    HttpUrl u;
    if ( !parse_http_url( url, u, why ) )
        return -1;
    if ( sock_.connect( u.host, u.port, why ) != 0 )
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
    if ( !jpeg_frame_check( pool_.data( 0 ), n, info ) )
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
    log_.info() << "net-src: connected " << url << " (" << width_ << 'x' << height_ << "), boundary " << parts_.boundary() << "\n";
    err_streak_ = 0;
    return 0;
}

void NetMjpegSource::teardown()
{
    sock_.close();
    primed_len_ = 0;
    pool_.release_all();
}

int NetMjpegSource::send_get( const std::string& host, int port, const std::string& path, std::string* why ) const
{
    // Streamed together, no printf-family (project rule 2).
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string( port )
        + "\r\nConnection: keep-alive\r\nAccept: multipart/x-mixed-replace, image/jpeg\r\nUser-Agent: light_ai/1.0\r\n\r\n";
    return sock_.send_all( req, why );
}

int NetMjpegSource::read_headers( std::string* why )
{
    // parse_http_head owns the grammar and the kHeadMax cap (a server that
    // drips headers forever is a data-volume cap, not a wall-clock one).
    while ( true )
    {
        const HttpHead h = parse_http_head( sock_.data(), sock_.size() );
        if ( h.status == HttpHead::Status::Ok )
        {
            // Cannot fail while the caps match (the static_assert above);
            // checked anyway, with the head parser's own text.
            if ( !parts_.reset( h.boundary, h.boundary_len ) )
            {
                if ( why )
                    *why = "multipart boundary too long";
                return -1;
            }
            sock_.consume( h.len );
            return 0;
        }
        if ( h.status == HttpHead::Status::Error )
        {
            if ( why )
                *why = h.why;
            return -1;
        }
        const BufferedSocket::Fill r = sock_.fill( kPollMs );
        if ( r == BufferedSocket::Fill::Interrupted || r == BufferedSocket::Fill::Data )
            continue;  // EINTR: transient, not a watchdog expiry
        if ( why )
            *why = "no HTTP response within the watchdog window";
        return -1;
    }
}

bool NetMjpegSource::part_buffered() const
{
    // A wedged-full window counts as ready: the read that follows desyncs.
    return sock_.full() || parts_.part_buffered( sock_.data(), sock_.size() );
}

void NetMjpegSource::drop_window()
{
    sock_.clear();
    parts_.resync();
}

int NetMjpegSource::poll_part_ready( int timeout_ms )
{
    if ( part_buffered() )
        return 1;
    int wait_ms = timeout_ms;
    while ( true )
    {
        const BufferedSocket::Fill r = sock_.fill( wait_ms );
        if ( r == BufferedSocket::Fill::Interrupted )
            return 0;  // EINTR: not ready yet and not a timeout -- caller retries
        if ( r == BufferedSocket::Fill::Closed )
            return -1;
        if ( r == BufferedSocket::Fill::Full )
            return kDesync;
        if ( part_buffered() )
            return 1;
        if ( r == BufferedSocket::Fill::Timeout )
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
        if ( frame_ok( pool_.data( 1 ), n, width_, height_ ) )
        {
            pool_.swap( 0, 1 );
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
    uint8_t* const dst = pool_.data( slot );
    while ( true )
    {
        const MultipartReader::Step st = parts_.feed( sock_.data(), sock_.size(), dst );
        sock_.consume( st.consumed );
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
        const BufferedSocket::Fill r = sock_.fill( kPollMs );
        if ( r == BufferedSocket::Fill::Interrupted || r == BufferedSocket::Fill::Data )
            continue;  // EINTR: transient, not a watchdog expiry
        parts_.resync();  // the watchdog, EOF / error, or a wedged-full window
        return r == BufferedSocket::Fill::Full ? kDesync : -1;
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
    const int scratch = pool_.free_slot( slot );
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
        if ( frame_ok( pool_.data( scratch ), next_n, width_, height_ ) )
        {
            pool_.swap( slot, scratch );
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
    if ( !sock_.is_open() )
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
        pool_.mark( 0 );
        if ( size )
            *size = primed_len_;
        primed_len_ = 0;
        err_streak_ = 0;
        return 0;
    }
    if ( !wait_part_ready( timeout_ms, err ) )
        return -1;
    int slot = pool_.free_slot();
    if ( slot < 0 )
    {
        // Contract: requeue() precedes the next dequeue(); a full pool
        // here is a caller bug — loud, then slot 0 defensively.
        log_.error() << "net-src: all buffers in flight, reusing slot 0 (pipeline contract violation)\n";
        slot = 0;
    }
    uint32_t n = 0;
    if ( const int rc = read_latest_part( slot, &n ); rc != 0 )
    {
        if ( err )
            *err = EIO;  // EOF / watchdog / recv error: recoverable
        return -1;
    }
    if ( !frame_ok( pool_.data( slot ), n, width_, height_ ) )
    {
        ++err_streak_;
        log_.warn() << "net-src: corrupt/incomplete JPEG frame (" << n << " bytes, streak " << err_streak_ << ") dropped\n";
        if ( err )
            *err = ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN;
        return -1;
    }
    err_streak_ = 0;
    pool_.mark( slot );
    if ( size )
        *size = n;
    return slot;
}

void NetMjpegSource::requeue( int index )
{
    pool_.release( index );
}

