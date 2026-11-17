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
    for ( size_t s = 0; head_len == kNpos; )
    {
        const size_t lf = line_scan::find_lf( p, s, n );
        if ( lf == kNpos )
            break;
        const size_t ce = line_scan::content_end( p, s, lf );
        if ( status_end == kNpos )
            status_end = ce;
        else if ( ce == s )
            head_len = lf + 1;
        else if ( ct == kNpos && line_scan::starts_with_ci( p + s, ce - s, "content-type:" ) )
        {
            ct = s;
            ct_end = ce;
        }
        s = lf + 1;
    }
    if ( head_len == kNpos )
    {
        if ( n >= static_cast<size_t>( HttpHead::kHeadMax ) )
            return error( "response headers oversized" );
        return HttpHead {};  // NeedMore
    }
    if ( line_scan::find_ci( p, status_end, " 200 " ) == kNpos )
        return error( "server did not answer 200" );
    if ( ct == kNpos )
        return error( "no multipart boundary in Content-Type" );
    HttpHead h = take_boundary( p + ct, ct_end - ct );
    if ( h.parse == HttpHead::Parse::Ok )
        h.len = head_len;
    return h;
}
