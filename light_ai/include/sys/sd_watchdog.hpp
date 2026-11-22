// SdWatchdog: dependency-free systemd NOTIFY_SOCKET client for WatchdogSec.
// Opens a non-blocking AF_UNIX SOCK_DGRAM socket at startup and sends
// "WATCHDOG=1" from the pipeline loop without heap allocations or blocking.

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/perf_clock.hpp"

class SdWatchdog
{
public:
    enum
    {
        kPingIntervalMs = 1000
    };

public:
    explicit SdWatchdog( const char* socket_path = std::getenv( "NOTIFY_SOCKET" ) ) noexcept
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

    SdWatchdog( const SdWatchdog& ) = delete;
    SdWatchdog& operator=( const SdWatchdog& ) = delete;
    SdWatchdog( SdWatchdog&& ) = delete;
    SdWatchdog& operator=( SdWatchdog&& ) = delete;

    bool active() const noexcept { return fd_ >= 0; }

    void notify_ready() noexcept
    {
        if ( fd_ < 0 )
            return;
        const char msg[] = "READY=1";
        ::sendto( fd_, msg, sizeof( msg ) - 1, MSG_NOSIGNAL | MSG_DONTWAIT,
                  reinterpret_cast<const sockaddr*>( &addr_ ), addr_len_ );
    }

    void ping_now( PerfClock::time_point now ) noexcept
    {
        if ( fd_ < 0 )
            return;
        const char msg[] = "WATCHDOG=1";
        ::sendto( fd_, msg, sizeof( msg ) - 1, MSG_NOSIGNAL | MSG_DONTWAIT,
                  reinterpret_cast<const sockaddr*>( &addr_ ), addr_len_ );
        last_ping_ = now;
        pinged_ = true;
    }

    void ping_if_due( PerfClock::time_point now ) noexcept
    {
        if ( fd_ < 0 )
            return;
        if ( !pinged_ || ms_between( last_ping_, now ) >= kPingIntervalMs )
            ping_now( now );
    }

private:
    int fd_ = -1;
    sockaddr_un addr_ {};
    socklen_t addr_len_ = 0;
    PerfClock::time_point last_ping_ {};
    bool pinged_ = false;
};
