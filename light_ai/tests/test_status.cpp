#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

#include "check.hpp"
#include "core/status.hpp"
#include "suites.hpp"

namespace
{
    std::string text( const Status& s )
    {
        std::ostringstream os;
        os << s;
        return os.str();
    }
}  // namespace

void test_status( TestRun& t )
{
    constexpr Status ok {};
    CHECK( t, ok.ok() && ok.code() == Errc::Ok && ok.kind() == Status::Sys::None && ok.sys() == 0 );
    CHECK_EQ( t, text( ok ), std::string( "ok" ) );

    const Status plain = make_status( Errc::Absent, "no usable USB capture node found" );
    CHECK( t, !plain.ok() && plain.code() == Errc::Absent && plain.kind() == Status::Sys::None );
    CHECK_EQ( t, text( plain ), std::string( "no usable USB capture node found" ) );

    const Status en = errno_status( Errc::Busy, "bind", EADDRINUSE );
    CHECK( t, !en.ok() && en.code() == Errc::Busy && en.kind() == Status::Sys::Errno && en.sys() == EADDRINUSE );
    CHECK_EQ( t, text( en ), std::string( "bind: " ) + std::strerror( EADDRINUSE ) );

    const Status sdk = sdk_status( Errc::Failed, "rknn_run", -5 );
    CHECK( t, !sdk.ok() && sdk.kind() == Status::Sys::Sdk && sdk.sys() == -5 );
    CHECK_EQ( t, text( sdk ), std::string( "rknn_run (rc -5)" ) );

    // ErrnoText alone: the same text as strerror, an unknown value included.
    std::ostringstream os;
    os << ErrnoText { ENODEV } << '|' << ErrnoText { 9999 };
    CHECK_EQ( t, os.str(), std::string( std::strerror( ENODEV ) ) + '|' + std::strerror( 9999 ) );
}
