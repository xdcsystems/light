// MultipartReader: the multipart/x-mixed-replace framing of an MJPEG pull
// stream as a pure state machine fed with bytes. No I/O: the caller keeps
// the receive window (core/byte_window.hpp), hands its unconsumed bytes to
// feed(), consumes what feed() parsed and refills on NeedMore.
// Host-tested on recorded streams (tests/test_multipart_reader.cpp).
//
// Grammar (lines end with LF, an optional CR before it: core/line_scan.hpp):
//   delimiter line -- the dash-boundary at a line start (after an LF, or
//                     at the start of the bytes while looking for a part),
//                     followed by a line end. The dash-boundary is the RFC
//                     form "--" + boundary; a declared boundary that already
//                     starts with "--" may also be sent bare. The RFC form
//                     is tried first at each position; the form of the
//                     first match is locked for the connection;
//   part header    -- lines up to the first empty line; Content-Length
//                     (a field at a line start, case-insensitive): digits
//                     with optional blanks around them, anything else = no
//                     Content-Length;
//   body           -- Content-Length bytes, or without it everything up to
//                     the line break before the next delimiter line (that
//                     line break, CRLF or LF, belongs to the delimiter and
//                     stays unconsumed for the next part).
// Bytes before a delimiter line are skipped (garbage between parts).
//
// The desync limit is the caller's window: while it looks for a delimiter
// or a header end, feed() consumes nothing, so a window that fills up
// without one is a desynced stream (NetMjpegSource). feed() reports Desync
// itself only for a body larger than body_cap. After Desync -- or after any
// read the caller abandons -- resync() and drop or keep the window.
//
// part_buffered() runs the same grammar on a copy of the state without
// copying: true iff feed() on the same bytes would end in Part or Desync
// (needs no more data). Thread: one owner at a time (the source's).

#pragma once

#include <cstddef>
#include <cstdint>

class MultipartReader
{
public:
    enum
    {
        kBoundaryMax = 60,  // HttpHead::kBoundaryMax
    };
    enum class Event : uint8_t
    {
        NeedMore,  // every byte that could be parsed is consumed; refill
        Part,      // one body complete in dst: size bytes
        Desync     // the stream does not parse: resync()
    };
    struct Step
    {
        Event ev = Event::NeedMore;
        size_t consumed = 0;  // bytes of data the caller drops
        uint32_t size = 0;    // Part: the body length
    };

public:
    // body_cap: the bytes dst holds (one frame slot).
    explicit MultipartReader( size_t body_cap ) : cap_( body_cap ) {}

    // A new connection: the boundary from the response head; the form is
    // unlocked again. false = empty or over kBoundaryMax (nothing parses).
    bool reset( const char* boundary, size_t len );
    // Back to "looking for a delimiter line"; the locked form stays.
    void resync();

    // Parses data[0, n) on from the current state; body bytes go to dst
    // (body_cap bytes, the same dst until the part is complete).
    Step feed( const uint8_t* data, size_t n, uint8_t* dst );
    // See the header comment. false before reset().
    bool part_buffered( const uint8_t* data, size_t n ) const;

    const char* boundary() const { return boundary_; }

private:
    enum class Phase : uint8_t
    {
        Delimiter,
        Header,
        Body
    };
    enum class Form : uint8_t
    {
        Open,  // not locked yet
        Rfc,   // "--" + boundary
        Bare   // boundary as declared (it starts with "--")
    };
    struct Cursor
    {
        Phase phase = Phase::Delimiter;
        Form form = Form::Open;
        long need = -1;  // Content-Length of the current part, -1 = none
        size_t got = 0;  // body bytes delivered so far
    };

private:
    // The one grammar: advances c over data, copying body bytes to dst
    // unless dst is null (part_buffered).
    Step run( Cursor& c, const uint8_t* data, size_t n, uint8_t* dst ) const;
    // One phase each; true = the phase advanced at off, false = out is the result.
    bool on_delimiter( Cursor& c, const uint8_t* d, size_t n, size_t& off, Step& out ) const;
    bool on_header( Cursor& c, const uint8_t* d, size_t n, size_t& off, Step& out ) const;
    Step on_body_length( Cursor& c, const uint8_t* d, size_t n, size_t off, uint8_t* dst ) const;
    Step on_body_scan( Cursor& c, const uint8_t* d, size_t n, size_t off, uint8_t* dst ) const;
    // The body is complete: Part with c.got bytes, back to Delimiter.
    Step finish( Cursor& c, size_t consumed ) const;
    // A delimiter line at p (p is a line start). Locks nothing: returns the
    // form that matched (Open = none) and the position past its line end.
    Form delimiter_at( const uint8_t* d, size_t n, size_t p, Form form, size_t& end ) const;
    // The first delimiter line in d[from, n) whose line start is after an LF
    // (or at from itself when at_from); false = none. form: in, the allowed
    // forms; out, the one that matched.
    bool find_delimiter( const uint8_t* d, size_t n, size_t from, bool at_from, Form& form, size_t& p, size_t& end ) const;

private:
    size_t cap_;
    Cursor cur_ {};
    char boundary_[kBoundaryMax + 1] = {};
    char dash_[kBoundaryMax + 3] = {};  // "--" + boundary: the RFC dash-boundary
    size_t blen_ = 0;
    bool bare_ok_ = false;  // the boundary starts with "--": the bare form is allowed
};
