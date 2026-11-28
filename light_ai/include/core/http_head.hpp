// parse_http_head: the HTTP response head of an MJPEG pull stream, parsed
// from the bytes buffered so far. Pure.
//
// Grammar: lines end with LF, an optional CR before it (core/line_scan.hpp);
// the head ends at the first empty line. Ok needs:
//   - the status line containing " 200 ";
//   - a Content-Type field with boundary=<value> (case-insensitive name;
//     optional blanks after '='; either a quoted-string "..." with a
//     closing '"' keeping internal spaces and ';', or an unquoted value
//     ending at ';' or the line end with trailing ' '/'\t' trimmed;
//     1..kBoundaryMax characters);
//   - no Transfer-Encoding field naming chunked (case-insensitive).
// NeedMore: no end of head in the bytes yet and fewer than kHeadMax of them.
// Error: err holds the reason (Errc::Invalid):
//   "response headers oversized" (kHeadMax bytes without an end of head),
//   "server did not answer 200", "chunked Transfer-Encoding is not supported",
//   "no multipart boundary in Content-Type",
//   "unterminated multipart boundary", "multipart boundary too long",
//   "empty multipart boundary".

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/status.hpp"

struct HttpHead
{
    enum
    {
        kHeadMax = 8 * 1024,  // a server that drips headers forever: a data-volume cap
        kBoundaryMax = 60,
    };
    enum class Parse : uint8_t
    {
        NeedMore,
        Ok,
        Error
    };

    Parse parse = Parse::NeedMore;
    size_t len = 0;  // Ok: bytes of the head, the empty line included
    char boundary[kBoundaryMax + 1] = {};  // Ok: NUL-terminated
    size_t boundary_len = 0;
    Status err {};  // Error: the reason
};

HttpHead parse_http_head( const uint8_t* p, size_t n );
