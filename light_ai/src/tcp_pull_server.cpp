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
        ssize_t n = 0;
        do
        {
            n = ::send( fd, buf, static_cast<size_t>( buf_size ), MSG_NOSIGNAL );
        } while ( n < 0 && errno == EINTR );
        if ( n > 0 )
            return static_cast<int>( n );
        if ( n == 0 )
            return AVERROR_EOF;  // errno is stale after a zero return: never read it here
        if ( errno == EPIPE || errno == ECONNRESET )
            return AVERROR( EPIPE );
        if ( errno == EAGAIN || errno == EWOULDBLOCK )
            return AVERROR( ETIMEDOUT );
        return AVERROR( EIO );
    }
}  // namespace

TcpPullServer::TcpPullServer( int port, int sndbuf_bytes, int sndtimeo_sec )
  : port_( port ), sndbuf_bytes_( sndbuf_bytes ), sndtimeo_sec_( sndtimeo_sec )
{}

TcpPullServer::~TcpPullServer()
{
    close_all();
}

bool TcpPullServer::listen()
{
    close_all();
    const int fd = ::socket( AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
    if ( fd < 0 )
        return false;
    int one = 1;
    ::setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( static_cast<uint16_t>( port_ ) );
    if ( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) < 0 || ::listen( fd, 1 ) < 0 )
    {
        ::close( fd );
        return false;
    }
    lfd_ = fd;
    return true;
}

bool TcpPullServer::accept_client( const std::atomic<bool>& quit )
{
    close_client();
    if ( lfd_ < 0 )
        return false;

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
            return false;
        }
        if ( pr == 0 )
            continue;
        if ( ( pf.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
            return false;
        const int cfd = ::accept4( lfd_, nullptr, nullptr, SOCK_CLOEXEC );
        if ( cfd >= 0 )
        {
            cfd_ = cfd;
            break;
        }
        if ( errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR )
            return false;
    }
    if ( quit.load() || cfd_ < 0 )
        return false;
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
    return true;
}

bool TcpPullServer::attach_avio( AVFormatContext* fc )
{
    if ( cfd_ < 0 || !fc )
        return false;
    iobuf_ = static_cast<uint8_t*>( av_malloc( kIoBufBytes ) );
    if ( !iobuf_ )
        return false;
    pb_ = avio_alloc_context( iobuf_, kIoBufBytes, 1, &cfd_, nullptr, write_packet, nullptr );
    if ( !pb_ )
    {
        av_freep( &iobuf_ );
        return false;
    }
    fc->pb = pb_;
    fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
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
