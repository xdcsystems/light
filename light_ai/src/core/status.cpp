#include <array>
#include <cstring>
#include <ostream>

#include "core/status.hpp"

// GNU strerror_r (g++ defines _GNU_SOURCE): returns the text, which is either
// a static string of the C library or the buffer -- never use buf directly.
template <class Stream>
Stream& write_errno( Stream& os, ErrnoText e )
{
    std::array<char, 128> buf {};
    return os << strerror_r( e.err, buf.data(), buf.size() );
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
