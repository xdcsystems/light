#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "check.hpp"
#include "core/http_head.hpp"
#include "core/multipart_reader.hpp"
#include "mjpeg_stream.hpp"
#include "suites.hpp"

namespace
{
    using Status = HttpHead::Status;

    HttpHead parse( const std::string& s )
    {
        return parse_http_head( reinterpret_cast<const uint8_t*>( s.data() ), s.size() );
    }

    bool error_is( const HttpHead& h, const char* why )
    {
        return h.status == Status::Error && std::strcmp( h.why, why ) == 0;
    }

    bool boundary_is( const HttpHead& h, const char* b )
    {
        return h.status == Status::Ok && h.boundary_len == std::strlen( b ) && std::strcmp( h.boundary, b ) == 0;
    }

    std::string head( const std::string& content_type, const std::string& eol = "\r\n" )
    {
        return "HTTP/1.1 200 OK" + eol + "Server: test" + eol + content_type + eol + "Cache-Control: no-cache" + eol + eol;
    }

    void heads( TestRun& t )
    {
        // Unquoted, CRLF; len stops at the empty line, the stream bytes after it stay.
        {
            const std::string h = head( "Content-Type: multipart/x-mixed-replace;boundary=Ba4oTvQMY8ew04N8dcnM" );
            const HttpHead r = parse( h + "--Ba4oTvQMY8ew04N8dcnM\r\n" );
            CHECK( t, boundary_is( r, "Ba4oTvQMY8ew04N8dcnM" ) );
            CHECK( t, r.len == h.size() );
        }
        // Bare LF.
        {
            const std::string h = head( "Content-Type: multipart/x-mixed-replace; boundary=frame", "\n" );
            const HttpHead r = parse( h );
            CHECK( t, boundary_is( r, "frame" ) && r.len == h.size() );
        }
        // Quoted, blanks after '=', a parameter after it; case-insensitive names.
        CHECK( t, boundary_is( parse( head( "content-TYPE: multipart/x-mixed-replace; BOUNDARY= \"my frame\"; x=1" ) ), "my frame" ) );
        CHECK( t, boundary_is( parse( head( "Content-Type: multipart/x-mixed-replace;boundary=--myboundary" ) ), "--myboundary" ) );
        // 60 characters fit, 61 fail loud, an empty one fails.
        CHECK( t, boundary_is( parse( head( "Content-Type: multipart/x-mixed-replace;boundary=" + std::string( 60, 'b' ) ) ),
                               std::string( 60, 'b' ).c_str() ) );
        CHECK( t, error_is( parse( head( "Content-Type: multipart/x-mixed-replace;boundary=" + std::string( 61, 'b' ) ) ),
                            "multipart boundary too long" ) );
        CHECK( t, error_is( parse( head( "Content-Type: multipart/x-mixed-replace;boundary=\"\"" ) ), "empty multipart boundary" ) );
        CHECK( t, error_is( parse( head( "Content-Type: multipart/x-mixed-replace;boundary=" ) ), "empty multipart boundary" ) );
        // The boundary counts only in Content-Type (change 3 of the plan).
        CHECK( t, error_is( parse( head( "X-Note: boundary=frame" ) ), "no multipart boundary in Content-Type" ) );
        CHECK( t, error_is( parse( head( "Content-Type: image/jpeg" ) ), "no multipart boundary in Content-Type" ) );
        // The status line must say 200.
        CHECK( t, error_is( parse( "HTTP/1.1 404 Not Found\r\nContent-Type: multipart/x-mixed-replace;boundary=f\r\n\r\n" ),
                            "server did not answer 200" ) );
        CHECK( t, error_is( parse( "HTTP/1.1 401 Unauthorized\r\nX: 200 \r\nContent-Type: multipart/x-mixed-replace;boundary=f\r\n\r\n" ),
                            "server did not answer 200" ) );
    }

    void need_more( TestRun& t )
    {
        const std::string h = head( "Content-Type: multipart/x-mixed-replace;boundary=frame" );
        // Every proper prefix needs more; the whole head parses.
        bool all_need_more = true;
        for ( size_t n = 0; n < h.size(); ++n )
            all_need_more = all_need_more && parse( h.substr( 0, n ) ).status == Status::NeedMore;
        CHECK( t, all_need_more );
        CHECK( t, parse( h ).status == Status::Ok );
        // The cap lives in the parser: kHeadMax bytes without an end of head is an Error.
        const std::string drip = "HTTP/1.1 200 OK\r\nX-Pad: " + std::string( HttpHead::kHeadMax, 'p' );
        CHECK( t, parse( drip.substr( 0, HttpHead::kHeadMax - 1 ) ).status == Status::NeedMore );
        CHECK( t, error_is( parse( drip.substr( 0, HttpHead::kHeadMax ) ), "response headers oversized" ) );
    }

    // The phone's real response head (IP Webcam), recorded once: see tests/data/README.
    void recorded( TestRun& t )
    {
        const mjpeg_test::Bytes rec = mjpeg_test::load_fixture( "ipwebcam_head.bin" );
        CHECK( t, !rec.empty() );
        const HttpHead r = parse_http_head( rec.data(), rec.size() );
        CHECK( t, boundary_is( r, "Ba4oTvQMY8ew04N8dcnM" ) );
        CHECK( t, r.len < rec.size() );
        if ( r.status != Status::Ok )
            return;
        // The rest of the recording: an extra CRLF, the delimiter, the first part
        // header (Content-Length: 64866). The body is not recorded: a stand-in
        // of that length completes the part.
        MultipartReader parts( 1 << 20 );
        CHECK( t, parts.reset( r.boundary, r.boundary_len ) );
        mjpeg_test::Bytes s( rec.begin() + static_cast<long>( r.len ), rec.end() );
        CHECK( t, !parts.part_buffered( s.data(), s.size() ) );
        s.resize( s.size() + 64866, 0x55 );
        CHECK( t, parts.part_buffered( s.data(), s.size() ) );
        mjpeg_test::Bytes dst( 1 << 20 );
        const MultipartReader::Step st = parts.feed( s.data(), s.size(), dst.data() );
        CHECK( t, st.ev == MultipartReader::Event::Part && st.size == 64866 && st.consumed == s.size() );
    }
}  // namespace

void test_http_head( TestRun& t )
{
    heads( t );
    need_more( t );
    recorded( t );
}
