#include <cerrno>
#include <cstring>
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
    int fail( std::string* why, const std::string& text )
    {
        if ( why )
            *why = text;
        return -1;
    }
}  // namespace

int BufferedSocket::connect( const std::string& host, int port, std::string* why )
{
    close();
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port_str = std::to_string( port );  // named: c_str() must outlive getaddrinfo
    addrinfo* res = nullptr;
    if ( getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res ) != 0 || res == nullptr )
        return fail( why, "addr resolution failed for " + host );
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
        return fail( why, std::string( "socket: " ) + strerror( e ) );
    }
    // Non-blocking connect with a kConnectMs window, then back to blocking I/O.
    const int fl = ::fcntl( fd_, F_GETFL, 0 );
    ::fcntl( fd_, F_SETFL, fl | O_NONBLOCK );
    const int cr = ::connect( fd_, res->ai_addr, res->ai_addrlen );
    const int ce = errno;
    freeaddrinfo( res );
    if ( cr != 0 && ce != EINPROGRESS )
    {
        close();
        return fail( why, std::string( "connect: " ) + strerror( ce ) );
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
            return fail( why, pr <= 0 ? std::string( "connect: timeout" ) : std::string( "connect: " ) + strerror( so_err ) );
        }
    }
    ::fcntl( fd_, F_SETFL, fl );
    int nd = 1;
    ::setsockopt( fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof( nd ) );
    return 0;
}

int BufferedSocket::send_all( const std::string& bytes, std::string* why ) const
{
    if ( fd_ < 0 )
        return fail( why, "send: not connected" );
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
            return fail( why, std::string( "send: " ) + ( w < 0 ? strerror( errno ) : "peer closed, 0 bytes sent" ) );
        }
        off += static_cast<size_t>( w );
    }
    return 0;
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
