// SdWatchdog: dependency-free systemd NOTIFY_SOCKET client for WatchdogSec.
// Opens a non-blocking AF_UNIX SOCK_DGRAM socket at startup and sends
// "WATCHDOG=1" from the pipeline loop without heap allocations or blocking.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/non_copyable.hpp"
#include "core/perf_clock.hpp"

namespace sd_watchdog_detail
{
inline double parse_ping_interval_ms( const char* watchdog_usec, double fallback_ms, double max_ms ) noexcept
{
    if ( watchdog_usec == nullptr || watchdog_usec[0] == '\0' )
        return fallback_ms;
    errno = 0;
    char* end = nullptr;
    const long long usec = std::strtoll( watchdog_usec, &end, 10 );
    if ( errno == ERANGE || end == watchdog_usec || end == nullptr || *end != '\0' || usec <= 0 )
        return fallback_ms;
    const double half_ms = static_cast<double>( usec ) / 2000.0;
    return std::clamp<double>( half_ms, 1.0, max_ms );
}
}  // namespace sd_watchdog_detail

class SdWatchdog : private NonMovable<SdWatchdog>
{
public:
    enum
    {
        kPingIntervalMs = 1000,
        kMaxPingIntervalMs = 60000
    };

public:
    explicit SdWatchdog( const char* socket_path = std::getenv( "NOTIFY_SOCKET" ),
                         const char* watchdog_usec = std::getenv( "WATCHDOG_USEC" ) ) noexcept
      : ping_interval_ms_( sd_watchdog_detail::parse_ping_interval_ms(
            watchdog_usec, static_cast<double>( kPingIntervalMs ), static_cast<double>( kMaxPingIntervalMs ) ) )
    {
        if ( socket_path == nullptr || ( socket_path[0] != '/' && socket_path[0] != '@' ) || socket_path[1] == '\0' )
            return;
        const std::size_t len = std::strlen( socket_path );
        if ( len >= sizeof( addr_.sun_path ) )
            return;

        fd_ = ::socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0 );
        if ( fd_ < 0 )
            return;

        addr_.sun_family = AF_UNIX;
        std::memcpy( addr_.sun_path, socket_path, len );
        if ( addr_.sun_path[0] == '@' )
            addr_.sun_path[0] = '\0';
        addr_len_ = static_cast<socklen_t>( offsetof( sockaddr_un, sun_path ) + len );
    }

    ~SdWatchdog()
    {
        if ( fd_ >= 0 )
            ::close( fd_ );
    }

    bool active() const noexcept { return fd_ >= 0; }
    double ping_interval_ms() const noexcept { return ping_interval_ms_; }

    void notify_ready() noexcept { send_msg( "READY=1", 7 ); }

    void notify_stopping() noexcept { send_msg( "STOPPING=1", 10 ); }

    void ping_now( PerfClock::time_point now ) noexcept
    {
        if ( fd_ < 0 )
            return;
        send_msg( "WATCHDOG=1", 10 );
        last_ping_ = now;
        pinged_ = true;
    }

    void ping_if_due( PerfClock::time_point now ) noexcept
    {
        if ( fd_ < 0 )
            return;
        if ( !pinged_ || ms_between( last_ping_, now ) >= ping_interval_ms_ )
            ping_now( now );
    }

private:
    void send_msg( const char* msg, std::size_t len ) noexcept
    {
        if ( fd_ < 0 )
            return;
        ::sendto( fd_, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT,
                  reinterpret_cast<const sockaddr*>( &addr_ ), addr_len_ );
    }

    int fd_ = -1;
    double ping_interval_ms_ = static_cast<double>( kPingIntervalMs );
    sockaddr_un addr_ {};
    socklen_t addr_len_ = 0;
    PerfClock::time_point last_ping_ {};
    bool pinged_ = false;
};
