#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include "tcp_pull_server.hpp"

namespace
{
    enum
    {
        kIoBufBytes = 32768,
        kPollMs = 200
    };

    int write_packet( void* opaque, const uint8_t* buf, int buf_size )
    {
        const int fd = *static_cast<const int*>( opaque );
        // Drain-the-chunk send: a partial return here is silent data loss --
        // flush_buffer (libavformat/aviobuf.c) checks only < 0 and resets
        // buf_ptr = buf_ptr_max = buffer, the unsent tail is never retried.
        // Partial sends are reachable: the accepted socket is blocking with
        // SO_SNDTIMEO, and send() returns the bytes already copied into the
        // kernel buffer when the 15 s window expires mid-chunk.
        size_t sent = 0;
        while ( sent < static_cast<size_t>( buf_size ) )
        {
            const ssize_t n = ::send( fd, buf + sent, static_cast<size_t>( buf_size ) - sent, MSG_NOSIGNAL );
            if ( n < 0 )
            {
                if ( errno == EINTR )
                    continue;
                if ( errno == EPIPE || errno == ECONNRESET )
                    return AVERROR( EPIPE );
                if ( errno == EAGAIN || errno == EWOULDBLOCK )
                    return AVERROR( ETIMEDOUT );
                return AVERROR( EIO );
            }
            if ( n == 0 )
                return AVERROR_EOF;  // errno is stale after a zero return: never read it here
            sent += static_cast<size_t>( n );
        }
        return static_cast<int>( sent );
    }
}  // namespace

TcpPullServer::TcpPullServer( int port, int sndbuf_bytes, int sndtimeo_sec )
  : port_( port ), sndbuf_bytes_( sndbuf_bytes ), sndtimeo_sec_( sndtimeo_sec )
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
            cfd_ = cfd;
            break;
        }
        if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
            return errno_status( Errc::Failed, "accept", errno );
    }
    if ( quit.load() || cfd_ < 0 )
        return make_status( Errc::Timeout, "no viewer" );
    const int cfd = cfd_;

    int one = 1;
    ::setsockopt( cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
    if ( sndbuf_bytes_ > 0 )
        ::setsockopt( cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes_, sizeof( sndbuf_bytes_ ) );
    if ( sndtimeo_sec_ > 0 )
    {
        const timeval tv { sndtimeo_sec_, 0 };
        ::setsockopt( cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
    }
    cfd_ = cfd;
    return {};
}

Status TcpPullServer::attach_avio( AVFormatContext* fc )
{
    if ( cfd_ < 0 || !fc )
        return make_status( Errc::Invalid, "no client" );
    iobuf_ = static_cast<uint8_t*>( av_malloc( kIoBufBytes ) );
    if ( !iobuf_ )
        return make_status( Errc::NoMemory, "avio buffer alloc failed" );
    pb_ = avio_alloc_context( iobuf_, kIoBufBytes, 1, &cfd_, nullptr, write_packet, nullptr );
    if ( !pb_ )
    {
        av_freep( &iobuf_ );
        return make_status( Errc::NoMemory, "avio_alloc_context failed" );
    }
    fc->pb = pb_;
    fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    return {};
}

void TcpPullServer::close_client() noexcept
{
    if ( pb_ )
    {
        av_freep( &pb_->buffer );
        avio_context_free( &pb_ );
        iobuf_ = nullptr;
    }
    else if ( iobuf_ )
    {
        av_freep( &iobuf_ );
    }
    if ( cfd_ >= 0 )
    {
        ::close( cfd_ );
        cfd_ = -1;
    }
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
