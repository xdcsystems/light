#include <cerrno>
#include <chrono>
#include <string>

#include "source/net_mjpeg_source.hpp"

#include "core/http_head.hpp"
#include "core/http_url.hpp"
#include "core/log.hpp"
#include "core/jpeg_probe.hpp"
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
// before MppDecoder / NetEncoder / RgaResizer are initialized. Every
// connect adopts the stream's size: a server restarted at another
// resolution re-attaches (SourceChannel re-initializes the decoder).
Status NetMjpegSource::prime_first_frame()
{
    uint32_t n = 0;
    if ( read_part( 0, &n ) != Read::Part )
        return make_status( Errc::Lost, "failed to read initial JPEG frame" );
    JpegSofInfo info {};
    if ( !jpeg_frame_check( pool_.data( 0 ), n, info ) )
        return make_status( Errc::Invalid, "initial JPEG frame is incomplete or missing SOFn" );
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
    // An IPv6 literal goes into Host: in brackets (RFC 3986 IP-literal).
    const bool bare_v6 = host.find( ':' ) != std::string::npos && ( host.front() != '[' || host.back() != ']' );
    const std::string host_field = bare_v6 ? '[' + host + ']' : host;
    // Streamed together, no printf-family (project rule 2).
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host_field + ":" + std::to_string( port )
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

// step_nonblocking_part: the first fill waits up to wait_ms, then the bytes
// already queued are drained without blocking. A Timeout or an Interrupted
// is no failure: the part stays pending for the next poll.
struct NetMjpegSource::PollWait
{
    void arm( const NetMjpegSource& /*src*/ ) const noexcept {}

    BufferedSocket::Fill next( NetMjpegSource& src )
    {
        const BufferedSocket::Fill r = src.sock_.fill( wait_ms );
        wait_ms = 0;  // after data: drain remaining queued bytes without blocking again
        return r;
    }

    bool pending( BufferedSocket::Fill r ) const noexcept
    {
        return r == BufferedSocket::Fill::Timeout || r == BufferedSocket::Fill::Interrupted;
    }

public:
    int wait_ms = 0;
};

// read_part. Three bounds, armed by what the reader has seen, never from entry:
//  - no part started (looking for a delimiter line, or past it with no header
//    byte yet) and between the end of the headers and the first body byte:
//    a plain kPollMs watchdog per fill (fill_until), so the idle gap before a
//    frame -- the previous part's delimiter line, or the next part's whole
//    header, may already sit in the window -- is not part of any deadline;
//  - the header lines: kPollMs in total from their first byte (a header
//    byte stays in the window until the header end is parsed);
//  - the body: kPartMs in total from its first byte (delivered to the slot,
//    or held back in the window by a body without Content-Length).
// Accepted limit: junk dripped before a delimiter line is bounded only by the
// window filling up (BufferedSocket Full -> Desync).
// No wait result is pending: every one but Data resyncs.
struct NetMjpegSource::PartDeadline
{
    using clk = std::chrono::steady_clock;

    void arm( const NetMjpegSource& src )
    {
        const bool bytes = src.sock_.size() > 0;
        if ( head == clk::time_point::max() && src.parts_.in_header() && bytes )
            head = clk::now() + std::chrono::milliseconds( kPollMs );
        if ( body == clk::time_point::max() && src.parts_.in_body() && ( bytes || src.parts_.body_bytes() > 0 ) )
            body = clk::now() + std::chrono::milliseconds( kPartMs );
    }

    BufferedSocket::Fill next( NetMjpegSource& src ) const
    {
        if ( body != clk::time_point::max() )
            return src.fill_before( body );
        if ( head != clk::time_point::max() && src.parts_.in_header() )
            return src.fill_before( head );
        return src.sock_.fill_until( kPollMs );
    }

    bool pending( BufferedSocket::Fill /*r*/ ) const noexcept { return false; }

public:
    clk::time_point head = clk::time_point::max();
    clk::time_point body = clk::time_point::max();
};

// wait.arm() runs right after each consume, before the events are read
// (PartDeadline arms its deadlines there).
template <typename Wait>
NetMjpegSource::Read NetMjpegSource::feed_part( int slot, uint32_t* size, Wait& wait )
{
    uint8_t* const dst = pool_.data( slot );
    BufferedSocket::Fill r = BufferedSocket::Fill::Data;
    while ( r == BufferedSocket::Fill::Data )
    {
        const MultipartReader::Step st = parts_.feed( sock_.data(), sock_.size(), dst );
        sock_.consume( st.consumed );
        wait.arm( *this );
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
        r = wait.next( *this );
    }
    if ( wait.pending( r ) )
        return Read::Pending;  // keep rx_slot_ and parts_ state for next poll
    parts_.resync();  // cancelled, watchdog / deadline, EOF / error, or a wedged-full window
    rx_slot_ = -1;
    return r == BufferedSocket::Fill::Full ? Read::Desync : Read::Lost;
}

NetMjpegSource::Read NetMjpegSource::step_nonblocking_part( int slot, int timeout_ms, uint32_t* size )
{
    PollWait wait { timeout_ms };
    return feed_part( slot, size, wait );
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
    PartDeadline wait {};
    return feed_part( slot, size, wait );
}

int NetMjpegSource::skip_buffered_parts( int slot, uint32_t& n )
{
    const int scratch = pool_.free_slot( slot );
    if ( scratch < 0 )
        return 0;
    int tail_bad = 0;
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
            tail_bad = 0;  // only the bad frames behind the newest good one count
        }
        else
        {
            ++tail_bad;
        }
        ++skip_parts_cnt_;
        pull_queued();
    }
    return tail_bad;
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

NetMjpegSource::Read NetMjpegSource::read_latest_part( int slot, uint32_t* size, int timeout_ms, int& tail_bad )
{
    tail_bad = 0;
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
    tail_bad = skip_buffered_parts( slot, n );
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
    int tail_bad = 0;
    const Read r = read_latest_part( slot, &n, timeout_ms, tail_bad );
    if ( r == Read::Pending )
        return dequeue_status( EAGAIN );
    if ( r == Read::Desync || r == Read::Oversized )
        return dequeue_status( ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN );
    if ( r != Read::Part )
        return dequeue_status( EIO );  // EOF / watchdog / recv error: recoverable
    if ( !frame_ok( pool_.data( slot ), n, width_, height_ ) )
    {
        // No good frame in the whole burst: the first part and the bad ones after it all count.
        err_streak_ += 1 + tail_bad;
        log_.warn() << "net-src: corrupt/incomplete JPEG frame (" << n << " bytes, streak " << err_streak_ << ") dropped\n";
        return dequeue_status( ( err_streak_ >= kErrStreakMax ) ? EIO : EAGAIN );
    }
    // The delivered frame is good: only the bad parts behind it stay in the streak.
    err_streak_ = tail_bad;
    last_cap_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch() )
                       .count();
    pool_.mark( slot );
    size = n;
    index = slot;
    return {};
}

Status NetMjpegSource::take_primed( int& index, uint32_t& size )
{
    pool_.mark( 0 );
    last_cap_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch() )
                       .count();
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

