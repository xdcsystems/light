#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "source/buffered_socket.hpp"

namespace
{
    // A failed connect() / SO_ERROR: nobody there is Absent (the prober retries).
    enum
    {
        kConnectSliceMs = 100
    };

    Status connect_error( int e )
    {
        const bool absent = e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH || e == EHOSTDOWN;
        return errno_status( absent ? Errc::Absent : Errc::Failed, "connect", e );
    }

    int resolve_ipv4( const std::string& host, const std::string& port_str, addrinfo*& res ) noexcept
    {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        if ( ::getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res ) == 0 && res != nullptr )
            return 0;
        hints.ai_flags = 0;
        return ::getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res );
    }
}  // namespace

bool BufferedSocket::cancelled() const noexcept
{
    return ( stop_ != nullptr && stop_->load( std::memory_order_acquire ) ) ||
           ( prober_quit_ != nullptr && prober_quit_->load( std::memory_order_acquire ) );
}

Status BufferedSocket::wait_connect() noexcept
{
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::milliseconds( kConnectMs );
    while ( !cancelled() )
    {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
        if ( rem <= 0 )
            return make_status( Errc::Timeout, "connect: timeout" );
        const int slice = static_cast<int>( std::min<long long>( rem, kConnectSliceMs ) );
        pollfd p { fd_, POLLOUT, 0 };
        const int pr = ::poll( &p, 1, slice );
        if ( pr == 0 || ( pr < 0 && errno == EINTR ) )
            continue;
        if ( pr < 0 )
            return errno_status( Errc::Failed, "connect: poll", errno );
        int so_err = 0;
        socklen_t slen = sizeof( so_err );
        if ( ::getsockopt( fd_, SOL_SOCKET, SO_ERROR, &so_err, &slen ) != 0 )
            return errno_status( Errc::Failed, "connect: getsockopt", errno );
        return so_err == 0 ? Status {} : connect_error( so_err );
    }
    return make_status( Errc::Timeout, "connect: cancelled" );
}

Status BufferedSocket::connect( const std::string& host, int port )
{
    close();
    if ( cancelled() )
        return make_status( Errc::Timeout, "connect: cancelled" );
    const std::string port_str = std::to_string( port );  // named: c_str() must outlive getaddrinfo
    addrinfo* res = nullptr;
    if ( resolve_ipv4( host, port_str, res ) != 0 || res == nullptr )
        return make_status( Errc::Absent, "addr resolution failed" );
    // SOCK_CLOEXEC: this fd must not leak into a fork()/exec() (this app
    // never forks, but a library or deploy wrapper might). The flag belongs
    // to the socket() type argument -- putting SOCK_CLOEXEC into
    // hints.ai_socktype makes getaddrinfo fail with EAI_SOCKTYPE, killing
    // the connection outright.
    fd_ = ::socket( res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol );
    if ( fd_ < 0 )
    {
        const int e = errno;
        freeaddrinfo( res );
        return errno_status( Errc::Failed, "socket", e );
    }
    // Non-blocking connect with a kConnectMs window, then back to blocking I/O.
    const int fl = ::fcntl( fd_, F_GETFL, 0 );
    if ( fl < 0 || ::fcntl( fd_, F_SETFL, fl | O_NONBLOCK ) != 0 )
    {
        const int e = errno;
        freeaddrinfo( res );
        close();
        return errno_status( Errc::Failed, "connect: fcntl", e );
    }
    const int cr = ::connect( fd_, res->ai_addr, res->ai_addrlen );
    const int ce = errno;
    freeaddrinfo( res );
    if ( cr != 0 && ce != EINPROGRESS )
    {
        close();
        return connect_error( ce );
    }
    if ( cr != 0 )
    {
        if ( Status wst = wait_connect(); !wst.ok() )
        {
            close();
            return wst;
        }
    }
    if ( ::fcntl( fd_, F_SETFL, fl ) != 0 )  // a socket left non-blocking would break the poll + recv contract
    {
        const int e = errno;
        close();
        return errno_status( Errc::Failed, "connect: fcntl", e );
    }
    int nd = 1;
    ::setsockopt( fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof( nd ) );
    return {};
}

Status BufferedSocket::send_all( const std::string& bytes ) const
{
    if ( fd_ < 0 )
        return make_status( Errc::Failed, "send: not connected" );
    size_t off = 0;
    while ( off < bytes.size() )
    {
        // MSG_NOSIGNAL: a mid-send disconnect must not raise SIGPIPE; the
        // flag keeps this call self-contained even if the global ignore is
        // ever removed.
        const ssize_t w = ::send( fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL );
        if ( w < 0 && errno == EINTR )
            continue;
        if ( w <= 0 )
        {
            // w == 0 on a blocking socket cannot happen for a nonzero
            // buffer, but errno is only set on failure: reading it after a
            // zero return would report a stale value from an unrelated call.
            return w < 0 ? errno_status( Errc::Lost, "send", errno ) : make_status( Errc::Lost, "send: peer closed, 0 bytes sent" );
        }
        off += static_cast<size_t>( w );
    }
    return {};
}

BufferedSocket::Fill BufferedSocket::fill( int timeout_ms )
{
    if ( fd_ < 0 )
        return Fill::Closed;
    uint8_t* const dst = win_.tail();  // compacts the unconsumed bytes to the front
    if ( win_.room() == 0 )
        return Fill::Full;
    pollfd p { fd_, POLLIN, 0 };
    const int pr = ::poll( &p, 1, timeout_ms >= 0 ? timeout_ms : 0 );
    if ( pr < 0 )
        return errno == EINTR ? Fill::Interrupted : Fill::Closed;
    if ( pr == 0 )
        return Fill::Timeout;
    const ssize_t n = ::recv( fd_, dst, win_.room(), 0 );
    if ( n < 0 && errno == EINTR )
        return Fill::Interrupted;  // interrupted before any data (e.g. a stop signal
                                   // arriving between poll() and recv()): not a
                                   // connection error and not a watchdog expiry
    if ( n <= 0 )
        return Fill::Closed;  // 0 = EOF, -1 = error: the connection is gone either way
    win_.commit( static_cast<size_t>( n ) );
    return Fill::Data;
}

BufferedSocket::Fill BufferedSocket::fill_until( int total_ms, int slice_ms )
{
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::milliseconds( total_ms );
    const int step_ms = slice_ms > 0 ? slice_ms : 200;
    while ( !cancelled() )
    {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
        if ( rem <= 0 )
            return Fill::Timeout;
        const Fill r = fill( static_cast<int>( std::min<long long>( rem, step_ms ) ) );
        if ( r == Fill::Timeout )
            continue;
        if ( r == Fill::Interrupted )
        {
            if ( cancelled() )
                return Fill::Interrupted;
            continue;
        }
        return r;
    }
    return Fill::Interrupted;
}

void BufferedSocket::close()
{
    if ( fd_ >= 0 )
    {
        ::close( fd_ );
        fd_ = -1;
    }
    win_.clear();
}
