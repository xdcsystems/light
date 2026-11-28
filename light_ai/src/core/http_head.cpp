#include <cstring>

#include "core/http_head.hpp"
#include "core/line_scan.hpp"

using line_scan::kNpos;

namespace
{
    HttpHead error( const char* why )
    {
        HttpHead h;
        h.parse = HttpHead::Parse::Error;
        h.err = make_status( Errc::Invalid, why );
        return h;
    }

    // The status line p[0, n) (CR LF excluded) answers 200: "HTTP/<version> 200[ <reason>]". The status code is the
    // second field, so a "200" inside a reason phrase does not count; the reason phrase may be empty (RFC 9112 s. 4).
    bool status_line_is_200( const uint8_t* p, size_t n ) noexcept
    {
        if ( !line_scan::starts_with_ci( p, n, "http/" ) )
            return false;
        size_t i = 5;  // past "HTTP/"
        while ( i < n && p[i] != ' ' )
            ++i;
        if ( i <= 5 || i >= n )
            return false;  // no version, or nothing after it
        while ( i < n && p[i] == ' ' )
            ++i;
        return i + 3 <= n && p[i] == '2' && p[i + 1] == '0' && p[i + 2] == '0' && ( i + 3 == n || p[i + 3] == ' ' );
    }

    bool is_ows( uint8_t c ) noexcept { return c == ' ' || c == '\t'; }

    // One element e[0, n) of a transfer-coding list: OWS, the coding name, optional parameters after OWS or ';'.
    bool names_chunked( const uint8_t* e, size_t n )
    {
        size_t s = 0;
        while ( s < n && is_ows( e[s] ) )
            ++s;
        size_t end = s;
        while ( end < n && !is_ows( e[end] ) && e[end] != ';' )
            ++end;
        return end - s == std::strlen( "chunked" ) && line_scan::starts_with_ci( e + s, end - s, "chunked" );
    }

    // A Transfer-Encoding field line v[0, n) whose comma-separated list names the chunked coding (RFC 9112 s. 6.1):
    // the whole coding name, so "x-chunked" or a parameter value "chunked" is another thing.
    bool chunked_coding_line( const uint8_t* v, size_t n )
    {
        if ( !line_scan::starts_with_ci( v, n, "transfer-encoding:" ) )
            return false;
        for ( size_t s = std::strlen( "transfer-encoding:" ); s < n; )
        {
            size_t e = s;
            while ( e < n && v[e] != ',' )
                ++e;
            if ( names_chunked( v + s, e - s ) )
                return true;
            s = e + 1;
        }
        return false;
    }

    // The boundary parameter inside one Content-Type line value v[0, n).
    // RFC 7230 BWS around "=" and RFC 2046 quoted-string: many cameras emit
    // boundary="xyz" (some with a blank after the '='); the closing quote is
    // a terminator so spaces and ';' inside quotes stay intact, whereas
    // unquoted values trim trailing horizontal whitespace (' ' / '\t') before
    // ';' or EOL. boundary="" is empty. An over-long value fails loud
    // instead of a silent truncation: a truncated pattern desyncs the stream.
    HttpHead take_boundary( const uint8_t* v, size_t n )
    {
        const size_t bpos = line_scan::find_ci( v, n, "boundary=" );
        if ( bpos == kNpos )
            return error( "no multipart boundary in Content-Type" );
        size_t s = bpos + 9;  // past "boundary="
        while ( s < n && ( v[s] == ' ' || v[s] == '\t' ) )
            ++s;
        const bool quoted = ( s < n && v[s] == '"' );
        if ( quoted )
            ++s;
        size_t e = s;
        while ( e < n && v[e] != '"' && ( quoted || v[e] != ';' ) )
            ++e;
        if ( quoted && ( e >= n || v[e] != '"' ) )
            return error( "unterminated multipart boundary" );
        if ( !quoted )
        {
            while ( e > s && ( v[e - 1] == ' ' || v[e - 1] == '\t' ) )
                --e;
        }
        const size_t len = e - s;
        if ( len > static_cast<size_t>( HttpHead::kBoundaryMax ) )
            return error( "multipart boundary too long" );
        if ( len == 0 )
            return error( "empty multipart boundary" );
        HttpHead h;
        h.parse = HttpHead::Parse::Ok;
        std::memcpy( h.boundary, v + s, len );
        h.boundary[len] = '\0';
        h.boundary_len = len;
        return h;
    }
}  // namespace

HttpHead parse_http_head( const uint8_t* p, size_t n )
{
    // The head ends at the first empty line; the status line is line one.
    size_t status_end = kNpos;  // content end of the status line
    size_t ct = kNpos;          // start / content end of the first Content-Type line
    size_t ct_end = 0;
    size_t head_len = kNpos;
    bool chunked = false;
    for ( size_t s = 0; head_len == kNpos; )
    {
        const size_t lf = line_scan::find_lf( p, s, n );
        if ( lf == kNpos )
            break;
        const size_t line = s;
        const size_t ce = line_scan::content_end( p, line, lf );
        s = lf + 1;
        if ( status_end == kNpos )
        {
            status_end = ce;
            continue;
        }
        if ( ce == line )
        {
            head_len = lf + 1;  // the blank line: the loop ends
            continue;
        }
        if ( ct == kNpos && line_scan::starts_with_ci( p + line, ce - line, "content-type:" ) )
        {
            ct = line;
            ct_end = ce;
        }
        chunked = chunked || chunked_coding_line( p + line, ce - line );
    }
    if ( head_len == kNpos )
    {
        if ( n >= static_cast<size_t>( HttpHead::kHeadMax ) )
            return error( "response headers oversized" );
        return HttpHead {};  // NeedMore
    }
    if ( !status_line_is_200( p, status_end ) )
        return error( "server did not answer 200" );
    // The multipart reader takes the body bytes as they come: chunk-size lines would land inside the frames.
    if ( chunked )
        return error( "chunked Transfer-Encoding is not supported" );
    if ( ct == kNpos )
        return error( "no multipart boundary in Content-Type" );
    HttpHead h = take_boundary( p + ct, ct_end - ct );
    if ( h.parse == HttpHead::Parse::Ok )
        h.len = head_len;
    return h;
}
