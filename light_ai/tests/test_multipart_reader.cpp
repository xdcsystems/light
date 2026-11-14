#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/multipart_reader.hpp"
#include "jpeg_probe.hpp"
#include "mjpeg_stream.hpp"
#include "suites.hpp"

using mjpeg_test::Bytes;
using mjpeg_test::Chunks;
using mjpeg_test::drive;
using mjpeg_test::DriveResult;
using mjpeg_test::make_stream;
using mjpeg_test::put;
using Event = MultipartReader::Event;

namespace
{
    constexpr size_t kCap = 4096;     // the slot of most cases
    constexpr size_t kWin = 4096;     // the window of most cases
    constexpr size_t kFrame = 1 << 20;  // NetMjpegSource::kFrameMax

    const Chunks kModes[] = { Chunks::Whole, Chunks::OneByte, Chunks::Random };

    MultipartReader reader( const std::string& boundary, size_t cap = kCap )
    {
        MultipartReader r( cap );
        r.reset( boundary.data(), boundary.size() );
        return r;
    }

    template <size_t N = kWin>
    DriveResult run( const std::string& boundary, const Bytes& s, Chunks mode, size_t cap = kCap )
    {
        MultipartReader r = reader( boundary, cap );
        return drive<N>( r, s, mode, cap );
    }

    // Bytes that contain no LF (so no delimiter line), from a fixed seed.
    Bytes garbage( size_t n, uint32_t seed )
    {
        std::mt19937 rng( seed );
        std::uniform_int_distribution<int> byte( 0, 255 );
        Bytes g( n );
        for ( auto& c : g )
        {
            c = static_cast<uint8_t>( byte( rng ) );
            if ( c == '\n' )
                c = 'x';
        }
        return g;
    }

    bool same_parts( const DriveResult& r, const std::vector<Bytes>& want )
    {
        return r.parts == want;
    }

    // Note 1 of the plan review: part_buffered( prefix ) == "feed on a copy
    // ends in Part or Desync", for every prefix, from the state r is in.
    bool buffered_matches_feed( const MultipartReader& r, const Bytes& s, size_t cap )
    {
        Bytes dst( cap );
        for ( size_t len = 0; len <= s.size(); ++len )
        {
            MultipartReader copy = r;
            const bool ready = copy.feed( s.data(), len, dst.data() ).ev != Event::NeedMore;
            if ( r.part_buffered( s.data(), len ) != ready )
                return false;
        }
        return true;
    }

    void clean_streams( TestRun& t, const Bytes& a, const Bytes& b )
    {
        const std::vector<Bytes> bodies { a, b, a };
        for ( const Chunks m : kModes )
        {
            // With and without Content-Length, CRLF and bare LF: the bodies come
            // out byte for byte (on CRLF, the old reader's bytes: the CRLF before
            // the delimiter is not body).
            for ( const bool with_len : { true, false } )
            {
                for ( const char* eol : { "\r\n", "\n" } )
                {
                    const Bytes s = make_stream( "--frame", bodies, with_len, eol );
                    const DriveResult r = run( "frame", s, m );
                    CHECK( t, same_parts( r, bodies ) );
                    CHECK( t, r.desyncs == 0 );
                }
            }
        }
        // The first delimiter without a leading line break, as IP Webcam
        // sends it, and with one (the RFC preamble form).
        {
            Bytes s;
            put( s, "\r\n" );
            put( s, make_stream( "--frame", bodies, true ) );
            CHECK( t, same_parts( run( "frame", s, Chunks::Random ), bodies ) );
        }
        // Content-Length with trailing blanks; a malformed one falls back to the
        // delimiter scan with the same body.
        for ( const char* cl : { "Content-Length: 377 \r\n", "content-length:\t377\r\n", "Content-Length: 37x\r\n" } )
        {
            Bytes s;
            put( s, std::string( "--frame\r\n" ) + cl + "\r\n" );
            put( s, a );
            put( s, "\r\n--frame\r\n" );
            const DriveResult r = run( "frame", s, Chunks::OneByte );
            CHECK( t, r.parts.size() == 1 && r.parts[0] == a );
        }
        // Content-Length only at a line start: "X-Content-Length" is another field.
        {
            Bytes s;
            put( s, "--frame\r\nX-Content-Length: 3\r\n\r\n" );
            put( s, a );
            put( s, "\r\n--frame\r\n" );
            const DriveResult r = run( "frame", s, Chunks::Whole );
            CHECK( t, r.parts.size() == 1 && r.parts[0] == a );
        }
        // An empty part header: the body starts after the empty line.
        {
            Bytes s;
            put( s, "--frame\r\n\r\n" );
            put( s, a );
            put( s, "\r\n--frame\r\n" );
            const DriveResult r = run( "frame", s, Chunks::Random );
            CHECK( t, r.parts.size() == 1 && r.parts[0] == a );
        }
    }

    void boundary_forms( TestRun& t, const Bytes& a, const Bytes& b )
    {
        const std::vector<Bytes> bodies { a, b };
        for ( const Chunks m : kModes )
        {
            for ( const bool with_len : { true, false } )
            {
                // Declared with a leading "--", sent bare: the bare form locks.
                CHECK( t, same_parts( run( "--frame", make_stream( "--frame", bodies, with_len ), m ), bodies ) );
                // Declared with a leading "--", sent in the RFC form: RFC first.
                CHECK( t, same_parts( run( "--frame", make_stream( "----frame", bodies, with_len ), m ), bodies ) );
            }
        }
        // RFC locked: a bare "--frame" line inside a body is body, not a delimiter.
        {
            Bytes body;
            put( body, "AAAA\r\n--frame\r\nBBBB" );
            const Bytes s = make_stream( "----frame", { body, a }, false );
            const DriveResult r = run( "--frame", s, Chunks::Random );
            CHECK( t, r.parts.size() == 2 && r.parts[0] == body && r.parts[1] == a );
        }
        // A delimiter counts only at a line start: "x--frame" is not one.
        {
            Bytes s;
            put( s, "x--frame\r\nContent-Length: 4\r\n\r\nJUNK\r\n" );
            put( s, make_stream( "--frame", { a }, true ) );
            const DriveResult r = run( "frame", s, Chunks::Whole );
            CHECK( t, r.parts.size() == 1 && r.parts[0] == a );
        }
        // reset() rejects what HttpHead never hands over.
        {
            MultipartReader r( kCap );
            CHECK( t, !r.reset( "", 0 ) );
            const std::string too_long( 61, 'b' );
            CHECK( t, !r.reset( too_long.data(), too_long.size() ) );
            CHECK( t, !r.part_buffered( reinterpret_cast<const uint8_t*>( "--b\r\n\r\n" ), 7 ) );
            const std::string max( 60, 'b' );
            CHECK( t, r.reset( max.data(), max.size() ) );
            CHECK( t, std::string( r.boundary() ) == max );
        }
    }

    void cut_streams( TestRun& t, const Bytes& a )
    {
        for ( const Chunks m : kModes )
        {
            // Cut in the second part's header: one part, the header pending.
            const std::string head2 = "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Le";
            Bytes s;
            put( s, "--frame\r\nContent-Length: " + std::to_string( a.size() ) + "\r\n\r\n" );
            put( s, a );
            put( s, head2 );
            const DriveResult r = run( "frame", s, m );
            CHECK( t, r.parts.size() == 1 && r.parts[0] == a && r.desyncs == 0 );
            CHECK( t, r.pending > 0 );
            // Cut in the second body, with and without Content-Length: no second part.
            for ( const bool with_len : { true, false } )
            {
                const Bytes f = make_stream( "--frame", { a, a }, with_len );
                const Bytes cut( f.begin(), f.begin() + static_cast<long>( f.size() - 60 ) );
                const DriveResult rc = run( "frame", cut, m );
                CHECK( t, rc.parts.size() == 1 && rc.parts[0] == a && rc.desyncs == 0 );
            }
        }
    }

    void broken_jpegs( TestRun& t, const Bytes& a )
    {
        // The reader delivers a part whatever it holds; the frame check drops it.
        Bytes no_eoi( a.begin(), a.end() - 2 );
        Bytes no_sof;
        {
            // Copy a without its SOF0 segment (FFC0 + length).
            for ( size_t i = 0; i < a.size(); )
            {
                if ( i + 3 < a.size() && a[i] == 0xFF && a[i + 1] == 0xC0 )
                {
                    i += 2 + ( ( static_cast<size_t>( a[i + 2] ) << 8 ) | a[i + 3] );
                    continue;
                }
                no_sof.push_back( a[i++] );
            }
        }
        CHECK( t, no_sof.size() < a.size() );
        const std::vector<Bytes> bodies { no_eoi, no_sof, a };
        const DriveResult r = run( "frame", make_stream( "--frame", bodies, false ), Chunks::Random );
        CHECK( t, same_parts( r, bodies ) );
        JpegSofInfo info;
        uint32_t n = static_cast<uint32_t>( r.parts[0].size() );
        CHECK( t, !jpeg_frame_check( r.parts[0].data(), n, info ) );
        n = static_cast<uint32_t>( r.parts[1].size() );
        CHECK( t, !jpeg_frame_check( r.parts[1].data(), n, info ) );
        n = static_cast<uint32_t>( r.parts[2].size() );
        CHECK( t, jpeg_frame_check( r.parts[2].data(), n, info ) && info.w == 16 && info.h == 16 );
    }

    void desync_and_resync( TestRun& t, const Bytes& a, const Bytes& b )
    {
        for ( const Chunks m : kModes )
        {
            // Short garbage between parts (lines included): skipped, no desync.
            {
                Bytes s = make_stream( "--frame", { a }, true, "\r\n", false );
                put( s, "garbage line\r\n" );
                put( s, garbage( 300, 1 ) );
                put( s, "\r\n" );
                put( s, make_stream( "--frame", { b }, true ) );
                const DriveResult r = run( "frame", s, m );
                CHECK( t, r.parts.size() == 2 && r.parts[0] == a && r.parts[1] == b && r.desyncs == 0 );
            }
            // Garbage longer than the window without a delimiter: desync, then
            // the next part intact.
            {
                Bytes s = make_stream( "--frame", { a }, true, "\r\n", false );
                put( s, garbage( kWin + 1000, 2 ) );
                put( s, "\r\n" );
                put( s, make_stream( "--frame", { b }, true ) );
                const DriveResult r = run( "frame", s, m );
                CHECK( t, r.parts.size() == 2 && r.parts[0] == a && r.parts[1] == b && r.desyncs >= 1 );
            }
            // An oversized part header (a field longer than the window): desync, resync.
            {
                Bytes s;
                put( s, "--frame\r\nX-Pad: " );
                put( s, garbage( kWin + 100, 3 ) );
                put( s, "\r\n\r\n" );
                put( s, a );
                put( s, "\r\n" );
                put( s, make_stream( "--frame", { b }, true ) );
                const DriveResult r = run( "frame", s, m );
                CHECK( t, r.parts.size() == 1 && r.parts[0] == b && r.desyncs >= 1 );
            }
            // A part larger than the slot, with Content-Length and without: desync,
            // later parts intact. The desync drops the whole window, the parts
            // buffered behind the big one with it (as in NetMjpegSource): enough
            // of them follow for the window to refill past the drop.
            for ( const bool with_len : { true, false } )
            {
                Bytes s = make_stream( "--frame", { garbage( kCap + 1, 4 ) }, with_len );
                put( s, make_stream( "--frame", std::vector<Bytes>( 12, b ), true ) );
                const DriveResult r = run( "frame", s, m );
                CHECK( t, !r.parts.empty() && r.parts.back() == b && r.desyncs >= 1 );
            }
        }
        // The real slot: exactly kFrameMax fits, one byte more desyncs (Content-Length).
        {
            const Bytes big = garbage( kFrame, 5 );
            const Bytes over = garbage( kFrame + 1, 6 );
            const DriveResult r = run( "frame", make_stream( "--frame", { big, a }, true ), Chunks::Whole, kFrame );
            CHECK( t, r.parts.size() == 2 && r.parts[0] == big && r.parts[1] == a && r.desyncs == 0 );
            Bytes s = make_stream( "--frame", { over }, true );
            put( s, make_stream( "--frame", { a }, true ) );
            const DriveResult ro = run( "frame", s, Chunks::Whole, kFrame );
            CHECK( t, ro.parts.size() == 1 && ro.parts[0] == a && ro.desyncs >= 1 );
        }
        // The production window (256 KiB): a no-Content-Length body larger than
        // the window streams through it; garbage beyond it desyncs.
        {
            const Bytes body = garbage( 300 * 1024, 8 );
            const DriveResult r = run<256 * 1024>( "frame", make_stream( "--frame", { body, a }, false ), Chunks::Random, kFrame );
            CHECK( t, r.parts.size() == 2 && r.parts[0] == body && r.parts[1] == a && r.desyncs == 0 );
            Bytes s = make_stream( "--frame", { a }, true, "\r\n", false );
            put( s, garbage( 256 * 1024 + 10, 9 ) );
            put( s, "\r\n" );
            put( s, make_stream( "--frame", { b }, true ) );
            const DriveResult rg = run<256 * 1024>( "frame", s, Chunks::Whole, kFrame );
            CHECK( t, rg.parts.size() == 2 && rg.parts[1] == b && rg.desyncs == 1 );
        }
    }

    void queued_parts( TestRun& t, const Bytes& a, const Bytes& b )
    {
        // Several parts in the window: part_buffered stays true until the last
        // one is consumed (the latest-frame skip loop relies on it).
        for ( const bool with_len : { true, false } )
        {
            const Bytes s = make_stream( "--frame", { a, b, a }, with_len );
            MultipartReader r = reader( "frame" );
            Bytes dst( kCap );
            size_t off = 0;
            int parts = 0;
            while ( r.part_buffered( s.data() + off, s.size() - off ) )
            {
                const MultipartReader::Step st = r.feed( s.data() + off, s.size() - off, dst.data() );
                CHECK( t, st.ev == Event::Part );
                off += st.consumed;
                ++parts;
            }
            CHECK( t, parts == 3 );
            CHECK( t, r.feed( s.data() + off, s.size() - off, dst.data() ).ev == Event::NeedMore );
        }
        // One grammar: for every prefix, part_buffered agrees with feed -- from a
        // fresh reader and from the state after the first part.
        const std::vector<Bytes> streams { make_stream( "--frame", { a, b }, true ),
                                           make_stream( "--frame", { a, b }, false ),
                                           make_stream( "--frame", { a, b }, false, "\n" ),
                                           make_stream( "----frame", { a, b }, true ) };
        for ( size_t i = 0; i < streams.size(); ++i )
        {
            const std::string bnd = i == 3 ? "--frame" : "frame";
            MultipartReader r = reader( bnd );
            CHECK( t, buffered_matches_feed( r, streams[i], kCap ) );
            Bytes dst( kCap );
            const MultipartReader::Step st = r.feed( streams[i].data(), streams[i].size(), dst.data() );
            CHECK( t, st.ev == Event::Part );
            const Bytes rest( streams[i].begin() + static_cast<long>( st.consumed ), streams[i].end() );
            CHECK( t, buffered_matches_feed( r, rest, kCap ) );
        }
        // An oversized Content-Length is "ready" too: the read that follows desyncs.
        {
            Bytes s;
            put( s, "--frame\r\nContent-Length: 5000\r\n\r\n" );
            CHECK( t, reader( "frame" ).part_buffered( s.data(), s.size() ) );
            CHECK( t, buffered_matches_feed( reader( "frame" ), s, kCap ) );
        }
    }
}  // namespace

void test_multipart_reader( TestRun& t )
{
    const Bytes a = mjpeg_test::load_fixture( "tiny_16x16.jpg" );
    const Bytes b = mjpeg_test::load_fixture( "tiny_32x16.jpg" );
    CHECK( t, a.size() > 100 && b.size() > 100 );
    if ( a.empty() || b.empty() )
        return;
    clean_streams( t, a, b );
    boundary_forms( t, a, b );
    cut_streams( t, a );
    broken_jpegs( t, a );
    desync_and_resync( t, a, b );
    queued_parts( t, a, b );
}
