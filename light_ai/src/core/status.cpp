#include <array>
#include <cstring>
#include <ostream>

#include "core/status.hpp"

namespace
{
    // strerror_r has two signatures; the overload picked by its return type reads the result of either one, so only
    // one of the two is used in a given build.
    // GNU (g++ defines _GNU_SOURCE): returns the text, a static string of the C library or the buffer.
    [[maybe_unused]] const char* strerror_text( const char* s, const char* ) noexcept { return s; }
    // XSI: returns 0 and writes the text into the buffer.
    [[maybe_unused]] const char* strerror_text( int rc, const char* buf ) noexcept { return rc == 0 ? buf : "unknown error"; }
}  // namespace

template <class Stream>
Stream& write_errno( Stream& os, ErrnoText e )
{
    std::array<char, 128> buf {};
    return os << strerror_text( strerror_r( e.err, buf.data(), buf.size() ), buf.data() );
}

std::ostream& operator<<( std::ostream& os, ErrnoText e ) { return write_errno( os, e ); }
Log::Line& operator<<( Log::Line& os, ErrnoText e ) { return write_errno( os, e ); }

template <class Stream>
Stream& write_status( Stream& os, const Status& s )
{
    if ( s.ok() )
        return os << "ok";
    os << s.what();
    switch ( s.kind() )
    {
        case Status::Sys::Errno:
            return os << ": " << ErrnoText { s.sys() };
        case Status::Sys::Sdk:
            return os << " (rc " << s.sys() << ')';
        case Status::Sys::None:
            break;
    }
    return os;
}

std::ostream& operator<<( std::ostream& os, const Status& s ) { return write_status( os, s ); }
Log::Line& operator<<( Log::Line& os, const Status& s ) { return write_status( os, s ); }
