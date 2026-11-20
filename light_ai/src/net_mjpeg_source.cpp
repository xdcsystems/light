#include <cerrno>
#include <chrono>
#include <string>

#include "net_mjpeg_source.hpp"

#include "core/http_head.hpp"
#include "core/http_url.hpp"
#include "core/log.hpp"
#include "jpeg_probe.hpp"
#include "source/source_types.hpp"  // dequeue_status

namespace
{
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

Status NetMjpegSource::setup_device( const std::string& url )
{
    teardown();

    // A failed attempt is logged here with its url: the prober's absent
    // line does not carry it, and this is the one place that knows it.
    const auto warn = [&]( Status st ) {
        log_.warn() << "net-src: " << url << ": " << st << '\n';
        return st;
    };

    HttpUrl u;
    if ( const Status st = parse_http_url( url, u ); !st.ok() )
        return warn( st );
    if ( const Status st = sock_.connect( u.host, u.port ); !st.ok() )
        return warn( st );
    Status st = send_get( u.host, u.port, u.path );
    if ( st.ok() )
        st = read_headers();
    if ( st.ok() )
        st = prime_first_frame();
    if ( !st.ok() )
    {
        teardown();
        return warn( st );
    }
    log_.info() << "net-src: connected " << url << " (" << width_ << 'x' << height_ << "), boundary " << parts_.boundary() << "\n";
    err_streak_ = 0;
    return {};
}

// Prime the first JPEG frame into slot 0 to read its SOFn geometry
// before MppDecoder / NetEncoder / RgaResizer are initialized.
Status NetMjpegSource::prime_first_frame()
{
    uint32_t n = 0;
    if ( read_part( 0, &n ) != Read::Part )
        return make_status( Errc::Lost, "failed to read initial JPEG frame" );
    JpegSofInfo info {};
    if ( !jpeg_frame_check( pool_.data( 0 ), n, info ) )
        return make_status( Errc::Invalid, "initial JPEG frame is incomplete or missing SOFn" );
    if ( width_ > 0 && height_ > 0 && ( info.w != width_ || info.h != height_ ) )
        return make_status( Errc::Unsupported, "stream resolution changed on reconnect" );
    width_ = info.w;
    height_ = info.h;
    primed_len_ = n;
    return {};
}

void NetMjpegSource::teardown() noexcept
{
    sock_.close();
    primed_len_ = 0;
    rx_slot_ = -1;
    pool_.release_all();
}

Status NetMjpegSource::send_get( const std::string& host, int port, const std::string& path ) const
{
    // Streamed together, no printf-family (project rule 2).
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string( port )
        + "\r\nConnection: keep-alive\r\nAccept: multipart/x-mixed-replace, image/jpeg\r\nUser-Agent: light_ai/1.0\r\n\r\n";
    return sock_.send_all( req );
}

BufferedSocket::Fill NetMjpegSource::fill_before( std::chrono::steady_clock::time_point deadline )
{
    const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - std::chrono::steady_clock::now() ).count();
    if ( rem <= 0 )
        return BufferedSocket::Fill::Timeout;
    return sock_.fill_until( static_cast<int>( rem < kPollMs ? rem : static_cast<long long>( kPollMs ) ) );
}

Status NetMjpegSource::read_headers()
{
    // parse_http_head owns the grammar and the kHeadMax byte cap; the
    // deadline caps the wall clock: a head dripped a byte per watchdog
    // window used to keep the prober (or the frame thread) here forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( kPollMs );
    HttpHead h = parse_http_head( sock_.data(), sock_.size() );
    while ( h.parse == HttpHead::Parse::NeedMore )
    {
        if ( fill_before( deadline ) != BufferedSocket::Fill::Data )
            return make_status( Errc::Timeout, "no HTTP response within the watchdog window" );
        h = parse_http_head( sock_.data(), sock_.size() );
    }
    if ( h.parse == HttpHead::Parse::Error )
        return h.err;
    // Cannot fail while the caps match (the static_assert above);
    // checked anyway: the reason comes from the reader itself.
    if ( Status rst = parts_.reset( h.boundary, h.boundary_len ); !rst.ok() )
        return rst;
    sock_.consume( h.len );
    return {};
}

bool NetMjpegSource::part_buffered() const
{
    return parts_.part_buffered( sock_.data(), sock_.size() );
}

void NetMjpegSource::pull_queued()
{
    while ( !part_buffered() )
    {
        if ( sock_.fill( 0 ) != BufferedSocket::Fill::Data )
            return;
    }
}

void NetMjpegSource::drop_window()
{
    sock_.clear();
    parts_.resync();
    rx_slot_ = -1;
}

int NetMjpegSource::acquire_rx_slot()
{
    if ( rx_slot_ >= 0 )
        return rx_slot_;
    int slot = pool_.free_slot();
    if ( slot < 0 )
    {
        // Contract: requeue() precedes the next dequeue(); a full pool
        // here is a caller bug — loud, then slot 0 defensively.
        log_.error() << "net-src: all buffers in flight, reusing slot 0 (pipeline contract violation)\n";
        slot = 0;
    }
    rx_slot_ = slot;
    return slot;
}

NetMjpegSource::Read NetMjpegSource::step_nonblocking_part( int slot, int timeout_ms, uint32_t* size )
{
    uint8_t* const dst = pool_.data( slot );
    int wait_ms = timeout_ms;
    BufferedSocket::Fill r = BufferedSocket::Fill::Data;
    while ( r == BufferedSocket::Fill::Data )
    {
        const MultipartReader::Step st = parts_.feed( sock_.data(), sock_.size(), dst );
        sock_.consume( st.consumed );
        if ( st.ev == MultipartReader::Event::Part )
        {
            *size = st.size;
            rx_slot_ = -1;
            return Read::Part;
        }
        if ( st.ev == MultipartReader::Event::Desync )
        {
            parts_.resync();
            rx_slot_ = -1;
            return Read::Oversized;
        }
        r = sock_.fill( wait_ms );
        wait_ms = 0;  // after data: drain remaining queued bytes without blocking again
    }
    if ( r == BufferedSocket::Fill::Timeout || r == BufferedSocket::Fill::Interrupted )
        return Read::Pending;  // keep rx_slot_ and parts_ state for next poll
    parts_.resync();
    rx_slot_ = -1;
    return r == BufferedSocket::Fill::Full ? Read::Desync : Read::Lost;
}

void NetMjpegSource::start()
{
    // Drain frames accumulated in the TCP socket while the Pipeline
    // constructor initialized NPU tensors, encoder and GPU drawer,
    // keeping only the freshest valid frame in slot 0 (primed_len_).
    // Read into slot 1 first so a failed/corrupt drain never clobbers
    // the valid primed frame already sitting in slot 0.
    for ( int guard = 0; guard < kDrainMax; ++guard )
    {
        uint32_t n = 0;
        const int slot = ( rx_slot_ >= 0 ) ? rx_slot_ : 1;
        rx_slot_ = slot;
        const Read rc = step_nonblocking_part( slot, 0, &n );
        if ( rc == Read::Oversized )
            continue;
        if ( rc != Read::Part )
        {
            if ( rc == Read::Desync )
                drop_window();
            break;
        }
        if ( frame_ok( pool_.data( slot ), n, width_, height_ ) )
        {
            pool_.swap( 0, slot );
            primed_len_ = n;
        }
    }
}

NetMjpegSource::Read NetMjpegSource::read_part( int slot, uint32_t* size )
{
    uint8_t* const dst = pool_.data( slot );
    // The part deadline runs from the part's first header byte, not from
    // entry: the idle gap before a frame (the previous part's delimiter line
    // may already sit in the window) stays a plain kPollMs watchdog
    // (fill_until). Once the delimiter is consumed, any byte left in the
    // window is part header or body, and the part is bounded to kPartMs in
    // total. Accepted limit: junk dripped before a delimiter line is bounded
    // only by the window filling up (BufferedSocket Full -> Desync).
    using clk = std::chrono::steady_clock;
    clk::time_point deadline = clk::time_point::max();
    BufferedSocket::Fill r = BufferedSocket::Fill::Data;
    while ( r == BufferedSocket::Fill::Data )
    {
        const MultipartReader::Step st = parts_.feed( sock_.data(), sock_.size(), dst );
        sock_.consume( st.consumed );
        if ( deadline == clk::time_point::max() && ( parts_.in_body() || ( parts_.in_header() && sock_.size() > 0 ) ) )
            deadline = clk::now() + std::chrono::milliseconds( kPartMs );
        if ( st.ev == MultipartReader::Event::Part )
        {
            *size = st.size;
            rx_slot_ = -1;
            return Read::Part;
        }
        if ( st.ev == MultipartReader::Event::Desync )
        {
            parts_.resync();
            rx_slot_ = -1;
            return Read::Oversized;
        }
        r = deadline == clk::time_point::max() ? sock_.fill_until( kPollMs ) : fill_before( deadline );
    }
    parts_.resync();  // cancelled, watchdog / deadline, EOF / error, or a wedged-full window
    rx_slot_ = -1;
    return r == BufferedSocket::Fill::Full ? Read::Desync : Read::Lost;
}

void NetMjpegSource::skip_buffered_parts( int slot, uint32_t& n )
{
    const int scratch = pool_.free_slot( slot );
    if ( scratch < 0 )
        return;
    // Pull any bytes already queued in the kernel socket buffer (0 ms poll)
    // so a multi-frame Wi-Fi burst is collapsed to the freshest complete frame.
    pull_queued();
    for ( int skip = 0; skip < kSkipMax && part_buffered(); ++skip )
    {
        uint32_t next_n = 0;
        rx_slot_ = scratch;
        const Read sr = step_nonblocking_part( scratch, 0, &next_n );
        if ( sr == Read::Oversized )
            continue;
        if ( sr != Read::Part )
        {
            if ( sr == Read::Desync )
                drop_window();
            break;
        }
        if ( frame_ok( pool_.data( scratch ), next_n, width_, height_ ) )
        {
            pool_.swap( slot, scratch );
            n = next_n;
        }
        ++skip_parts_cnt_;
        pull_queued();
    }
}

bool NetMjpegSource::note_bad_part( Read rc )
{
    if ( rc != Read::Desync && rc != Read::Oversized )
        return false;
    // Wedged window (Desync) drops the buffer; Oversized already drained to the
    // next delimiter so buffered bytes stay intact.
    ++err_streak_;
    if ( rc == Read::Desync )
        drop_window();
    return true;
}

NetMjpegSource::Read NetMjpegSource::read_latest_part( int slot, uint32_t* size, int timeout_ms )
{
    uint32_t n = 0;
    const bool blocking = timeout_ms < 0;
    Read rc = blocking ? read_part( slot, &n ) : step_nonblocking_part( slot, timeout_ms, &n );
    // Only retry when blocking or a complete next part is already buffered.
    if ( note_bad_part( rc ) && err_streak_ < kErrStreakMax && ( blocking || part_buffered() ) )
    {
        n = 0;
        rc = blocking ? read_part( slot, &n ) : step_nonblocking_part( slot, 0, &n );
        note_bad_part( rc );  // a bad retry counts too: kErrStreakMax bad parts in a row -> EIO
    }
    if ( rc != Read::Part )
        return rc;
    skip_buffered_parts( slot, n );
    *size = n;
    return Read::Part;
}

Status NetMjpegSource::dequeue( int& index, uint32_t& size, int timeout_ms ) noexcept
{
    if ( !sock_.is_open() )
        return dequeue_status( ENODEV );
    if ( err_streak_ >= kErrStreakMax )
        return dequeue_status( EIO );  // recoverable stall -> the reconnect ladder
    if ( primed_len_ > 0 )
        return take_primed( index, size );
    const int slot = acquire_rx_slot();
    uint32_t n = 0;
    const Read r = read_latest_part( slot, &n, timeout_ms );
    if ( r == Read::Pending )
        return dequeue_status( EAGAIN );
    if ( r == Read::Desync || r == Read::Oversized )
        return dequeue_status( ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN );
    if ( r != Read::Part )
        return dequeue_status( EIO );  // EOF / watchdog / recv error: recoverable
    if ( !frame_ok( pool_.data( slot ), n, width_, height_ ) )
    {
        ++err_streak_;
        log_.warn() << "net-src: corrupt/incomplete JPEG frame (" << n << " bytes, streak " << err_streak_ << ") dropped\n";
        return dequeue_status( ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN );
    }
    err_streak_ = 0;
    pool_.mark( slot );
    size = n;
    index = slot;
    return {};
}

Status NetMjpegSource::take_primed( int& index, uint32_t& size )
{
    pool_.mark( 0 );
    size = primed_len_;
    index = 0;
    primed_len_ = 0;
    err_streak_ = 0;
    return {};
}

void NetMjpegSource::requeue( int index ) noexcept
{
    pool_.release( index );
}

