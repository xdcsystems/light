#include <chrono>
#include <cstddef>
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

namespace
{
    // The next datagram on the non-blocking server socket; empty = none queued.
    std::string next_datagram( int srv )
    {
        char buf[64] {};
        const ssize_t n = ::recv( srv, buf, sizeof( buf ) - 1, 0 );
        return n > 0 ? std::string( buf, static_cast<std::size_t>( n ) ) : std::string();
    }

    // READY=1 / STOPPING=1 need only the socket; WATCHDOG=1 needs a positive
    // WATCHDOG_USEC and, when WATCHDOG_PID is set, this process's pid.
    void check_keepalive_case( TestRun& t, int srv, const std::string& name, const char* usec, const char* pid,
                               bool expect_ping )
    {
        SdWatchdog wd( name.c_str(), usec, pid );
        CHECK( t, wd.active() );
        CHECK_EQ( t, wd.pinging(), expect_ping );
        wd.notify_ready();
        CHECK_STR_EQ( t, next_datagram( srv ), "READY=1" );
        const auto t0 = PerfClock::now();
        wd.ping_now( t0 );
        CHECK_STR_EQ( t, next_datagram( srv ), expect_ping ? "WATCHDOG=1" : "" );
        wd.ping_if_due( t0 + std::chrono::milliseconds( 5000 ) );
        CHECK_STR_EQ( t, next_datagram( srv ), expect_ping ? "WATCHDOG=1" : "" );
        wd.notify_stopping();
        CHECK_STR_EQ( t, next_datagram( srv ), "STOPPING=1" );
    }

    void check_keepalive_gating( TestRun& t )
    {
        const std::string name = "@light_ai_wd_gate_" + std::to_string( ::getpid() );
        const int srv = ::socket( AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0 );
        CHECK( t, srv >= 0 );
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::memcpy( addr.sun_path, name.data(), name.size() );
        addr.sun_path[0] = '\0';
        const socklen_t len = static_cast<socklen_t>( offsetof( sockaddr_un, sun_path ) + name.size() );
        CHECK_EQ( t, ::bind( srv, reinterpret_cast<const sockaddr*>( &addr ), len ), 0 );

        const std::string own_pid = std::to_string( ::getpid() );
        const std::string other_pid = std::to_string( ::getpid() + 1 );
        check_keepalive_case( t, srv, name, nullptr, nullptr, false );
        check_keepalive_case( t, srv, name, "", nullptr, false );
        check_keepalive_case( t, srv, name, "0", nullptr, false );
        check_keepalive_case( t, srv, name, "2000000", nullptr, true );
        check_keepalive_case( t, srv, name, "2000000", "", true );
        check_keepalive_case( t, srv, name, "2000000", own_pid.c_str(), true );
        check_keepalive_case( t, srv, name, "2000000", other_pid.c_str(), false );
        check_keepalive_case( t, srv, name, "2000000", "not-a-pid", false );
        ::close( srv );
    }
}  // namespace

void test_sd_watchdog( TestRun& t )
{
    // Disabled when socket path is empty or null.
    {
        SdWatchdog wd( nullptr );
        CHECK( t, !wd.active() );
        wd.notify_ready();
        wd.notify_stopping();
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
        SdWatchdog wd( sock_path.c_str(), "2000000", nullptr );
        CHECK( t, wd.active() );
        CHECK( t, wd.pinging() );
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

    // Abstract UNIX DGRAM socket (@name) receives WATCHDOG=1.
    {
        const std::string abs_name = "@light_ai_wd_abs_" + std::to_string( ::getpid() );
        const int abs_srv = ::socket( AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0 );
        CHECK( t, abs_srv >= 0 );
        sockaddr_un abs_addr {};
        abs_addr.sun_family = AF_UNIX;
        std::memcpy( abs_addr.sun_path, abs_name.data(), abs_name.size() );
        abs_addr.sun_path[0] = '\0';
        const socklen_t abs_len = static_cast<socklen_t>( offsetof( sockaddr_un, sun_path ) + abs_name.size() );
        CHECK_EQ( t, ::bind( abs_srv, reinterpret_cast<const sockaddr*>( &abs_addr ), abs_len ), 0 );

        SdWatchdog wd( abs_name.c_str(), "2000000", nullptr );
        CHECK( t, wd.active() );
        wd.notify_ready();
        char rbuf[32] {};
        const ssize_t rn = ::recv( abs_srv, rbuf, sizeof( rbuf ) - 1, 0 );
        CHECK_EQ( t, rn, static_cast<ssize_t>( 7 ) );
        CHECK_STR_EQ( t, std::string( rbuf ), "READY=1" );
        wd.ping_now( PerfClock::now() );

        char buf[32] {};
        const ssize_t n = ::recv( abs_srv, buf, sizeof( buf ) - 1, 0 );
        CHECK_EQ( t, n, static_cast<ssize_t>( 10 ) );
        CHECK_STR_EQ( t, std::string( buf ), "WATCHDOG=1" );

        wd.notify_stopping();
        char sbuf[32] {};
        const ssize_t sn = ::recv( abs_srv, sbuf, sizeof( sbuf ) - 1, 0 );
        CHECK_EQ( t, sn, static_cast<ssize_t>( 10 ) );
        CHECK_STR_EQ( t, std::string( sbuf ), "STOPPING=1" );
        ::close( abs_srv );
    }

    // WATCHDOG_USEC / 2 parsing with small-value half, ERANGE, and invalid fallback.
    {
        const SdWatchdog wd_half( nullptr, "600000", nullptr );
        CHECK_NEAR( t, wd_half.ping_interval_ms(), 300.0, 1e-6 );
        const SdWatchdog wd_small( nullptr, "100000", nullptr );
        CHECK_NEAR( t, wd_small.ping_interval_ms(), 50.0, 1e-6 );
        const SdWatchdog wd_overflow( nullptr, "99999999999999999999999999999", nullptr );
        CHECK_NEAR( t, wd_overflow.ping_interval_ms(), 1000.0, 1e-6 );
        const SdWatchdog wd_bad( nullptr, "not-a-number", nullptr );
        CHECK_NEAR( t, wd_bad.ping_interval_ms(), 1000.0, 1e-6 );
    }

    check_keepalive_gating( t );
}

