// parse_http_head: the HTTP response head of an MJPEG pull stream, parsed
// from the bytes buffered so far. Pure, host-tested (tests/test_http_head.cpp).
//
// Grammar: lines end with LF, an optional CR before it (core/line_scan.hpp);
// the head ends at the first empty line. Ok needs:
//   - the status line containing " 200 ";
//   - a Content-Type field with boundary=<value> (case-insensitive name;
//     optional blanks after '='; an optional opening quote; the value ends
//     at '"', ';' or the line end; 1..kBoundaryMax characters).
// NeedMore: no end of head in the bytes yet and fewer than kHeadMax of them.
// Error: why holds the reason -- the texts of the old read_headers:
//   "response headers oversized" (kHeadMax bytes without an end of head),
//   "server did not answer 200", "no multipart boundary in Content-Type",
//   "multipart boundary too long", "empty multipart boundary".

#pragma once

#include <cstddef>
#include <cstdint>

struct HttpHead
{
    enum
    {
        kHeadMax = 8 * 1024,  // a server that drips headers forever: a data-volume cap
        kBoundaryMax = 60,
    };
    enum class Status : uint8_t
    {
        NeedMore,
        Ok,
        Error
    };

    Status status = Status::NeedMore;
    size_t len = 0;  // Ok: bytes of the head, the empty line included
    char boundary[kBoundaryMax + 1] = {};  // Ok: NUL-terminated
    size_t boundary_len = 0;
    const char* why = "";  // Error: the reason
};

HttpHead parse_http_head( const uint8_t* p, size_t n );
