#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "check.hpp"
#include "core/perf_clock.hpp"
#include "suites.hpp"
#include "sys/sd_watchdog.hpp"

void test_sd_watchdog( TestRun& t )
{
    // Disabled when socket path is empty or null.
    {
        SdWatchdog wd( nullptr );
        CHECK( t, !wd.active() );
        wd.ping_now( PerfClock::now() );
    }

    // Active UNIX DGRAM socket receives WATCHDOG=1 and throttles within kPingIntervalMs.
    const std::string sock_path = "/tmp/light_ai_wd_test_" + std::to_string( ::getpid() ) + ".sock";
    ::unlink( sock_path.c_str() );

    const int srv = ::socket( AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0 );
    CHECK( t, srv >= 0 );
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy( addr.sun_path, sock_path.c_str(), sizeof( addr.sun_path ) - 1 );
    const int bind_rc = ::bind( srv, reinterpret_cast<const sockaddr*>( &addr ), sizeof( addr ) );
    CHECK_EQ( t, bind_rc, 0 );

    {
        SdWatchdog wd( sock_path.c_str() );
        CHECK( t, wd.active() );
        const auto t0 = PerfClock::now();
        wd.ping_if_due( t0 );
        wd.ping_if_due( t0 + std::chrono::milliseconds( 200 ) );  // throttled

        char buf[64] {};
        const ssize_t n1 = ::recv( srv, buf, sizeof( buf ) - 1, 0 );
        CHECK_EQ( t, n1, static_cast<ssize_t>( 10 ) );
        CHECK_STR_EQ( t, std::string( buf ), "WATCHDOG=1" );

        const ssize_t n2 = ::recv( srv, buf, sizeof( buf ) - 1, 0 );
        CHECK( t, n2 < 0 );  // no second packet yet

        wd.ping_if_due( t0 + std::chrono::milliseconds( 1100 ) );
        std::memset( buf, 0, sizeof( buf ) );
        const ssize_t n3 = ::recv( srv, buf, sizeof( buf ) - 1, 0 );
        CHECK_EQ( t, n3, static_cast<ssize_t>( 10 ) );
        CHECK_STR_EQ( t, std::string( buf ), "WATCHDOG=1" );
    }

    ::close( srv );
    ::unlink( sock_path.c_str() );
}
