// MultipartReader: the multipart/x-mixed-replace framing of an MJPEG pull
// stream as a pure state machine fed with bytes. No I/O: the caller keeps
// the receive window (core/byte_window.hpp), feeds its unconsumed bytes,
// consumes what feed() parsed and refills on NeedMore.
//
// Grammar (lines end with LF, an optional CR before it: core/line_scan.hpp):
//   delimiter line -- at a line start: the RFC form "--" + boundary, or a
//                     declared boundary already starting with "--" bare;
//                     the RFC form is tried first and the form of the
//                     first match is locked;
//   part header    -- lines up to the first empty line; Content-Length
//                     (a line-start field, case-insensitive): digits
//                     with optional blanks, anything else = absent;
//   body           -- Content-Length bytes, or everything up to the line
//                     break before the next delimiter (that break belongs
//                     to the delimiter and stays unconsumed).
// Bytes before a delimiter line are skipped (garbage between parts).
//
// Desync is the caller's window: while searching for a delimiter or a
// header end feed() consumes nothing, so a full window without one is a
// desynced stream (NetMjpegSource). For a body above body_cap, feed()
// drains and discards the oversized part across NeedMore chunks, then
// reports Desync once at the end of the part (next delimiter intact).
// After Desync -- or an abandoned read -- resync(), then drop or keep
// the window.
//
// part_buffered() runs the same grammar on a state copy without copying:
// true iff feed() on the same bytes would end in Part or Desync.
// Thread: one owner at a time (the source's).

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/status.hpp"

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
    Status reset( const char* boundary, size_t len );
    // Back to "looking for a delimiter line"; the locked form stays.
    void resync();

    // Parses data[0, n) on from the current state; body bytes go to dst
    // (body_cap bytes, the same dst until the part is complete).
    Step feed( const uint8_t* data, size_t n, uint8_t* dst );
    // See the header comment. false before reset().
    bool part_buffered( const uint8_t* data, size_t n ) const;

    const char* boundary() const { return boundary_; }
    // A part's delimiter and headers are parsed: its body is arriving.
    bool in_body() const noexcept { return cur_.phase == Phase::Body; }
    // Past a delimiter line, reading the part's header lines.
    bool in_header() const noexcept { return cur_.phase == Phase::Header; }

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
        long need = -1;     // Content-Length of the current part, -1 = none
        size_t got = 0;     // body bytes delivered (or skipped) so far
        bool skip = false;  // true while draining an oversized (> body_cap) part
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
