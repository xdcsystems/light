#include <cerrno>
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
    Status connect_error( int e )
    {
        const bool absent = e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH || e == EHOSTDOWN;
        return errno_status( absent ? Errc::Absent : Errc::Failed, "connect", e );
    }
}  // namespace

Status BufferedSocket::connect( const std::string& host, int port )
{
    close();
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port_str = std::to_string( port );  // named: c_str() must outlive getaddrinfo
    addrinfo* res = nullptr;
    if ( getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res ) != 0 || res == nullptr )
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
        pollfd p { fd_, POLLOUT, 0 };
        const int pr = ::poll( &p, 1, kConnectMs );
        int so_err = 0;
        socklen_t slen = sizeof( so_err );
        if ( pr <= 0 || ::getsockopt( fd_, SOL_SOCKET, SO_ERROR, &so_err, &slen ) != 0 || so_err != 0 )
        {
            close();
            return pr <= 0 ? make_status( Errc::Timeout, "connect: timeout" ) : connect_error( so_err );
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

void BufferedSocket::close()
{
    if ( fd_ >= 0 )
    {
        ::close( fd_ );
        fd_ = -1;
    }
    win_.clear();
}
