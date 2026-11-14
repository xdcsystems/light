#include <algorithm>
#include <climits>
#include <cstring>

#include "core/line_scan.hpp"
#include "core/multipart_reader.hpp"

using line_scan::kNpos;

namespace
{
    // A Content-Length value v[0, n) (the field content after the colon):
    // optional blanks, digits, optional blanks (RFC 7230 OWS: some camera
    // servers emit "Content-Length: 12345 \r\n"), then the line end. No sign,
    // nothing else, no overflow; -1 otherwise (the part is read without it).
    long parse_length( const uint8_t* v, size_t n )
    {
        size_t p = 0;
        while ( p < n && ( v[p] == ' ' || v[p] == '\t' ) )
            ++p;
        if ( p >= n || v[p] < '0' || v[p] > '9' )
            return -1;
        long x = 0;
        while ( p < n && v[p] >= '0' && v[p] <= '9' )
        {
            if ( x > ( LONG_MAX - ( v[p] - '0' ) ) / 10 )
                return -1;  // overflow: not a sane Content-Length
            x = x * 10 + ( v[p] - '0' );
            ++p;
        }
        while ( p < n && ( v[p] == ' ' || v[p] == '\t' ) )
            ++p;
        return p == n ? x : -1;
    }

    MultipartReader::Step make_step( MultipartReader::Event ev, size_t consumed, uint32_t size = 0 )
    {
        MultipartReader::Step s;
        s.ev = ev;
        s.consumed = consumed;
        s.size = size;
        return s;
    }
}  // namespace

bool MultipartReader::reset( const char* boundary, size_t len )
{
    cur_ = Cursor {};
    blen_ = 0;
    bare_ok_ = false;
    boundary_[0] = '\0';
    if ( len == 0 || len > static_cast<size_t>( kBoundaryMax ) )
        return false;
    std::memcpy( boundary_, boundary, len );
    boundary_[len] = '\0';
    std::memcpy( dash_, "--", 2 );
    std::memcpy( dash_ + 2, boundary, len );
    dash_[len + 2] = '\0';
    blen_ = len;
    bare_ok_ = len > 2 && boundary[0] == '-' && boundary[1] == '-';
    return true;
}

void MultipartReader::resync()
{
    const Form f = cur_.form;
    cur_ = Cursor {};
    cur_.form = f;
}

MultipartReader::Step MultipartReader::feed( const uint8_t* data, size_t n, uint8_t* dst )
{
    if ( blen_ == 0 )
        return make_step( Event::Desync, 0 );
    return run( cur_, data, n, dst );
}

bool MultipartReader::part_buffered( const uint8_t* data, size_t n ) const
{
    if ( blen_ == 0 )
        return false;
    Cursor c = cur_;
    return run( c, data, n, nullptr ).ev != Event::NeedMore;
}

MultipartReader::Step MultipartReader::run( Cursor& c, const uint8_t* data, size_t n, uint8_t* dst ) const
{
    size_t off = 0;
    Step out;
    while ( c.phase != Phase::Body )
    {
        const bool advanced = c.phase == Phase::Delimiter ? on_delimiter( c, data, n, off, out )
                                                          : on_header( c, data, n, off, out );
        if ( !advanced )
            return out;
    }
    return c.need >= 0 ? on_body_length( c, data, n, off, dst ) : on_body_scan( c, data, n, off, dst );
}

bool MultipartReader::on_delimiter( Cursor& c, const uint8_t* d, size_t n, size_t& off, Step& out ) const
{
    Form f = c.form;
    size_t p = 0;
    size_t end = 0;
    if ( !find_delimiter( d, n, off, true, f, p, end ) )
    {
        out = make_step( Event::NeedMore, off );  // garbage stays until the window fills: the desync limit
        return false;
    }
    c.form = f;  // locked for the connection
    c.phase = Phase::Header;
    off = end;
    return true;
}

bool MultipartReader::on_header( Cursor& c, const uint8_t* d, size_t n, size_t& off, Step& out ) const
{
    long need = -1;
    bool seen = false;  // the first Content-Length field decides
    size_t s = off;     // the current line start
    while ( true )
    {
        const size_t lf = line_scan::find_lf( d, s, n );
        if ( lf == kNpos )
        {
            out = make_step( Event::NeedMore, off );  // the header stays until it is complete
            return false;
        }
        const size_t ce = line_scan::content_end( d, s, lf );
        if ( ce == s )
        {
            off = lf + 1;  // the empty line: the header ends
            break;
        }
        if ( !seen && line_scan::starts_with_ci( d + s, ce - s, "content-length:" ) )
        {
            seen = true;
            need = parse_length( d + s + 15, ce - s - 15 );  // past "content-length:"
        }
        s = lf + 1;
    }
    c.need = need;
    c.got = 0;
    c.phase = Phase::Body;
    if ( need > 0 && static_cast<size_t>( need ) > cap_ )
    {
        out = make_step( Event::Desync, off );  // an oversized "part": desynced garbage
        return false;
    }
    return true;
}

MultipartReader::Step MultipartReader::finish( Cursor& c, size_t consumed ) const
{
    const auto size = static_cast<uint32_t>( c.got );
    c.phase = Phase::Delimiter;
    c.need = -1;
    c.got = 0;
    return make_step( Event::Part, consumed, size );
}

MultipartReader::Step MultipartReader::on_body_length( Cursor& c, const uint8_t* d, size_t n, size_t off, uint8_t* dst ) const
{
    // The window is refilled as it drains: the caller's watchdog covers
    // the whole body, not one chunk.
    const size_t take = std::min( n - off, static_cast<size_t>( c.need ) - c.got );
    if ( dst != nullptr )
        std::memcpy( dst + c.got, d + off, take );
    c.got += take;
    off += take;
    if ( c.got == static_cast<size_t>( c.need ) )
        return finish( c, off );
    return make_step( Event::NeedMore, off );
}

MultipartReader::Step MultipartReader::on_body_scan( Cursor& c, const uint8_t* d, size_t n, size_t off, uint8_t* dst ) const
{
    // No Content-Length: copy the body while looking for the next delimiter
    // line. The line break before it (CRLF or LF) belongs to the delimiter
    // and stays unconsumed, so on_delimiter() finds the delimiter at the
    // next line start.
    Form f = c.form;
    size_t p = 0;
    size_t end = 0;
    if ( find_delimiter( d, n, off, false, f, p, end ) )
    {
        size_t lb = p - 1;  // the LF before the delimiter
        if ( lb > off && d[lb - 1] == '\r' )
            --lb;
        const size_t len = lb - off;
        if ( c.got + len > cap_ )
            return make_step( Event::Desync, off );
        if ( dst != nullptr )
            std::memcpy( dst + c.got, d + off, len );
        c.got += len;
        return finish( c, lb );
    }
    // Keep the longest partial delimiter (CR LF "--" boundary CR) in the
    // window, so a delimiter split across two refills is never cut in half
    // and its CR never lands in the body.
    const size_t keep = blen_ + 5;
    const size_t avail = n - off;
    if ( avail > keep )
    {
        const size_t flush = avail - keep;
        if ( c.got + flush > cap_ )
            return make_step( Event::Desync, off );
        if ( dst != nullptr )
            std::memcpy( dst + c.got, d + off, flush );
        c.got += flush;
        off += flush;
    }
    return make_step( Event::NeedMore, off );
}

MultipartReader::Form MultipartReader::delimiter_at( const uint8_t* d, size_t n, size_t p, Form form, size_t& end ) const
{
    const size_t rest = n - p;
    if ( form != Form::Bare && rest >= blen_ + 2 && std::memcmp( d + p, dash_, blen_ + 2 ) == 0 )
    {
        end = line_scan::eol_at( d, p + blen_ + 2, n );
        if ( end != kNpos )
            return Form::Rfc;
    }
    if ( form != Form::Rfc && bare_ok_ && rest >= blen_ && std::memcmp( d + p, boundary_, blen_ ) == 0 )
    {
        end = line_scan::eol_at( d, p + blen_, n );
        if ( end != kNpos )
            return Form::Bare;
    }
    return Form::Open;
}

bool MultipartReader::find_delimiter( const uint8_t* d, size_t n, size_t from, bool at_from, Form& form, size_t& p,
                                      size_t& end ) const
{
    size_t q = from;  // a line start
    if ( !at_from )
    {
        const size_t lf = line_scan::find_lf( d, from, n );
        if ( lf == kNpos )
            return false;
        q = lf + 1;
    }
    while ( q < n )
    {
        const Form m = delimiter_at( d, n, q, form, end );
        if ( m != Form::Open )
        {
            form = m;
            p = q;
            return true;
        }
        const size_t lf = line_scan::find_lf( d, q, n );
        if ( lf == kNpos )
            return false;
        q = lf + 1;
    }
    return false;
}
