#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform/tcp_pull_server.hpp"

namespace
{
    enum
    {
        kPollMs = 200
    };

    void configure_client_socket( int cfd, int sndbuf_bytes, int sndtimeo_sec ) noexcept
    {
        int one = 1;
        ::setsockopt( cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
        if ( sndbuf_bytes > 0 )
            ::setsockopt( cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes, sizeof( sndbuf_bytes ) );
        if ( sndtimeo_sec > 0 )
        {
            const timeval tv { sndtimeo_sec, 0 };
            ::setsockopt( cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
        }
    }
}  // namespace

TcpPullServer::TcpPullServer( int port, int sndbuf_bytes, int sndtimeo_sec )
  : port_( port ), sndbuf_bytes_( sndbuf_bytes ), sndtimeo_sec_( sndtimeo_sec ), write_ctx_( sndtimeo_sec )
{}

TcpPullServer::~TcpPullServer()
{
    close_all();
}

Status TcpPullServer::listen()
{
    close_all();
    const int fd = ::socket( AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
    if ( fd < 0 )
        return errno_status( Errc::Failed, "socket", errno );
    int one = 1;
    ::setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( static_cast<uint16_t>( port_ ) );
    if ( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) < 0 )
    {
        const int e = errno;
        ::close( fd );
        return errno_status( e == EADDRINUSE ? Errc::Busy : Errc::Failed, "bind", e );
    }
    if ( ::listen( fd, 1 ) < 0 )
    {
        const int e = errno;
        ::close( fd );
        return errno_status( Errc::Failed, "listen", e );
    }
    lfd_ = fd;
    return {};
}

Status TcpPullServer::accept_client( const std::atomic<bool>& quit )
{
    close_client();
    if ( lfd_ < 0 )
        return make_status( Errc::Invalid, "no listener" );

    pollfd pf {};
    pf.fd = lfd_;
    pf.events = POLLIN;
    while ( !quit.load() )
    {
        const int pr = ::poll( &pf, 1, kPollMs );
        if ( pr < 0 )
        {
            if ( errno == EINTR )
                continue;
            return errno_status( Errc::Failed, "poll", errno );
        }
        if ( pr == 0 )
            continue;
        if ( ( pf.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
            return make_status( Errc::Failed, "listener error (poll revents)" );
        const int cfd = ::accept4( lfd_, nullptr, nullptr, SOCK_CLOEXEC );
        if ( cfd >= 0 )
        {
            if ( !adopt_client( cfd ) )
                return make_status( Errc::Timeout, "no viewer" );
            break;
        }
        if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
            return errno_status( Errc::Failed, "accept", errno );
    }
    const int cfd = write_ctx_.cfd.load( std::memory_order_acquire );
    if ( quit.load() || cfd < 0 )
        return make_status( Errc::Timeout, "no viewer" );

    configure_client_socket( cfd, sndbuf_bytes_, sndtimeo_sec_ );
    return {};
}

bool TcpPullServer::adopt_client( int cfd ) noexcept
{
    const std::lock_guard<std::mutex> lk( cfd_mx_ );
    if ( aborted_ )
    {
        ::close( cfd );
        return false;
    }
    write_ctx_.cfd.store( cfd, std::memory_order_release );
    return true;
}

void TcpPullServer::close_client() noexcept
{
    int fd = -1;
    {
        const std::lock_guard<std::mutex> lk( cfd_mx_ );
        fd = write_ctx_.cfd.exchange( -1, std::memory_order_acq_rel );
    }
    if ( fd >= 0 )
        ::close( fd );
}

void TcpPullServer::abort_client() noexcept
{
    const std::lock_guard<std::mutex> lk( cfd_mx_ );
    aborted_ = true;
    const int fd = write_ctx_.cfd.load( std::memory_order_acquire );
    if ( fd >= 0 )
        ::shutdown( fd, SHUT_RDWR );
}

void TcpPullServer::close_all() noexcept
{
    close_client();
    if ( lfd_ >= 0 )
    {
        ::close( lfd_ );
        lfd_ = -1;
    }
}
