// Line helpers shared by the HTTP head parser (core/http_head.hpp) and the
// multipart reader (core/multipart_reader.hpp): one definition of a line
// end for both -- LF, with an optional CR before it (CRLF or bare LF).
// Every helper is bounded by n: the buffers are socket windows, never
// NUL-terminated.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace line_scan
{

constexpr size_t kNpos = static_cast<size_t>( -1 );

// Position of the first LF in d[from, n), kNpos when absent.
inline size_t find_lf( const uint8_t* d, size_t from, size_t n )
{
    if ( from >= n )
        return kNpos;
    const void* const hit = std::memchr( d + from, '\n', n - from );
    return hit == nullptr ? kNpos : static_cast<size_t>( static_cast<const uint8_t*>( hit ) - d );
}

// End of the line content for a line whose LF is at lf (the CR before it,
// if any, is not content). s: the line start, so s <= result <= lf.
inline size_t content_end( const uint8_t* d, size_t s, size_t lf )
{
    return ( lf > s && d[lf - 1] == '\r' ) ? lf - 1 : lf;
}

// A line end at q: CRLF or LF. Returns the position just past it, kNpos
// when d[q] starts no line end or the CRLF is cut by n.
inline size_t eol_at( const uint8_t* d, size_t q, size_t n )
{
    if ( q < n && d[q] == '\n' )
        return q + 1;
    if ( q + 1 < n && d[q] == '\r' && d[q + 1] == '\n' )
        return q + 2;
    return kNpos;
}

inline bool eq_ci( uint8_t a, char b )
{
    return std::tolower( a ) == std::tolower( static_cast<unsigned char>( b ) );
}

// d[0, len) starts with lit, ASCII case-insensitive.
inline bool starts_with_ci( const uint8_t* d, size_t len, const char* lit )
{
    const size_t k = std::strlen( lit );
    return len >= k && std::equal( d, d + k, lit, []( uint8_t a, char b ) { return eq_ci( a, b ); } );
}

// First position of lit in d[0, len), ASCII case-insensitive; kNpos when absent.
inline size_t find_ci( const uint8_t* d, size_t len, const char* lit )
{
    const size_t k = std::strlen( lit );
    if ( k == 0 )
        return 0;
    const uint8_t* const end = d + len;
    const uint8_t* const hit = std::search( d, end, lit, lit + k, []( uint8_t a, char b ) { return eq_ci( a, b ); } );
    return hit == end ? kNpos : static_cast<size_t>( hit - d );
}

}  // namespace line_scan
