// core/line_scan.hpp: the bounded line helpers of the HTTP head parser and
// the multipart reader (socket windows: never NUL-terminated).

#include <cstdint>
#include <cstring>

#include "core/line_scan.hpp"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    const uint8_t* bytes( const char* s )
    {
        return reinterpret_cast<const uint8_t*>( s );
    }
}  // namespace

void test_line_scan( TestRun& t )
{
    using namespace line_scan;
    const char* txt = "ab\r\ncd\nef";
    const uint8_t* d = bytes( txt );
    const size_t n = std::strlen( txt );

    // find_lf: bounded by n and from.
    CHECK( t, find_lf( d, 0, n ) == 3 );
    CHECK( t, find_lf( d, 4, n ) == 6 );
    CHECK( t, find_lf( d, 7, n ) == kNpos );
    CHECK( t, find_lf( d, n, n ) == kNpos && find_lf( d, n + 5, n ) == kNpos );
    CHECK( t, find_lf( d, 0, 3 ) == kNpos );  // the LF just past the window is not seen

    // content_end: a CR before the LF is not content; a bare LF line keeps its end.
    CHECK( t, content_end( d, 0, 3 ) == 2 );
    CHECK( t, content_end( d, 4, 6 ) == 6 );
    CHECK( t, content_end( bytes( "\n" ), 0, 0 ) == 0 );  // empty line: s == lf

    // eol_at: LF, CRLF, a CRLF cut by n, no line end.
    CHECK( t, eol_at( d, 2, n ) == 4 );
    CHECK( t, eol_at( d, 3, n ) == 4 );
    CHECK( t, eol_at( d, 6, n ) == 7 );
    CHECK( t, eol_at( d, 2, 3 ) == kNpos );  // CR at the window end: incomplete
    CHECK( t, eol_at( d, 0, n ) == kNpos );
    CHECK( t, eol_at( d, n, n ) == kNpos );

    // Case-insensitive helpers, bounded by len.
    const uint8_t* h = bytes( "Content-Type: multipart/x-mixed-replace; BOUNDARY=frame" );
    const size_t hl = std::strlen( reinterpret_cast<const char*>( h ) );
    CHECK( t, starts_with_ci( h, hl, "content-type:" ) );
    CHECK( t, !starts_with_ci( h, 5, "content" ) );  // shorter than the literal
    CHECK( t, starts_with_ci( h, hl, "" ) );
    CHECK( t, find_ci( h, hl, "boundary=" ) == 41 );
    CHECK( t, find_ci( h, hl, "missing" ) == kNpos );
    CHECK( t, find_ci( h, 40, "boundary=" ) == kNpos );  // past len is not searched
    CHECK( t, find_ci( h, hl, "" ) == 0 );
    CHECK( t, eq_ci( 'A', 'a' ) && !eq_ci( 'A', 'b' ) );
}
