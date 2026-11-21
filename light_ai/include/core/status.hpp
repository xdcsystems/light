// Status: the result of an operation that can fail for a reason (README,
// "Rules of the project", Errors).
//
// What it holds:
//   code  -- the class of the failure, the one a caller branches on (Errc);
//   what  -- the reason, a string literal (static storage, never owned or freed);
//   sys   -- errno or an SDK return code (Sys says which), 0 when there is none.
// The context (node, host, port, path) is not in it: the caller that knows it
// prints it next to the Status. Trivially copyable, no heap, no statics; SDK-free.
//
// Printed with operator<< (src/core/status.cpp):
//   errno:  "<what>: <strerror_r(sys)>"
//   SDK:    "<what> (rc <sys>)"
//   plain:  "<what>"
//   ok:     "ok"
// ErrnoText prints one errno the same way (strerror_r: strerror is not
// thread-safe, and the prober, the frame thread and the encoder thread all log).

#pragma once

#include <cstdint>
#include <ostream>

#include "core/log.hpp"

// The failure classes. Only the ones a caller branches on, plus Failed.
enum class Errc : uint8_t
{
    Ok,
    Absent,       // not there: no node / file / usable device / reachable peer
    Lost,         // was there, now gone: ENODEV / EIO mid-stream, EOF, a watchdog stall
    Busy,         // EBUSY, EADDRINUSE
    Timeout,      // nothing within the wait (EAGAIN / EINTR on a poll): retry
    Unsupported,  // a mode / format / layout the program does not handle
    NoMemory,     // an allocation (dma-buf, mpp buffer, mmap)
    Invalid,      // a bad value (url, argument)
    Failed        // anything else; sys() says what
};

class [[nodiscard]] Status
{
public:
    // Where sys() comes from.
    enum class Sys : uint8_t
    {
        None,
        Errno,
        Sdk
    };

    constexpr Status() = default;  // ok
    constexpr Status( Errc code, const char* what, Sys kind = Sys::None, int sys = 0 )
      : code_( code ), kind_( kind ), sys_( sys ), what_( what )
    {
    }

    constexpr bool ok() const { return code_ == Errc::Ok; }
    constexpr Errc code() const { return code_; }
    constexpr const char* what() const { return what_; }
    constexpr Sys kind() const { return kind_; }
    constexpr int sys() const { return sys_; }

private:
    Errc code_ = Errc::Ok;
    Sys kind_ = Sys::None;
    int sys_ = 0;
    const char* what_ = "ok";
};

// Factories (free functions: no static members, README). Named apart from the
// classes' own fail() helpers, which would hide a free fail() inside members.
constexpr Status make_status( Errc code, const char* what )
{
    return { code, what };
}
constexpr Status errno_status( Errc code, const char* what, int err )
{
    return { code, what, Status::Sys::Errno, err };
}
constexpr Status sdk_status( Errc code, const char* what, int rc )
{
    return { code, what, Status::Sys::Sdk, rc };
}

// One errno as text, thread-safe: os << ErrnoText { e }.
struct ErrnoText
{
    int err = 0;
};

std::ostream& operator<<( std::ostream& os, ErrnoText e );
std::ostream& operator<<( std::ostream& os, const Status& s );
Log::Line& operator<<( Log::Line& os, ErrnoText e );
Log::Line& operator<<( Log::Line& os, const Status& s );
