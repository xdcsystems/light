// Shared ffmpeg and TCP pull transport helpers across net_* files.
#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C"
{
#include <libavutil/error.h>
}

inline std::string av_err_str( int ret )
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror( ret, buf, sizeof( buf ) );
    return std::string( buf );
}

inline void av_noop_free( void*, uint8_t* ) {}

// avio write callback over an accepted TCP client socket.
inline int tcp_avio_write_cb( void* opaque, const uint8_t* buf, int buf_size )
{
    const int fd = *static_cast<const int*>( opaque );
    const ssize_t n = ::send( fd, buf, static_cast<size_t>( buf_size ), MSG_NOSIGNAL );
    if ( n > 0 )
        return static_cast<int>( n );
    if ( errno == EPIPE || errno == ECONNRESET )
        return AVERROR( EPIPE );
    if ( errno == EAGAIN || errno == EWOULDBLOCK )
        return AVERROR( ETIMEDOUT );
    return AVERROR( EIO );
}

inline int open_tcp_listener( int port )
{
    const int fd = ::socket( AF_INET, SOCK_STREAM, 0 );
    if ( fd < 0 )
        return -1;
    int one = 1;
    ::setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl( INADDR_ANY );
    addr.sin_port = htons( static_cast<uint16_t>( port ) );
    if ( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) < 0 || ::listen( fd, 1 ) < 0 )
    {
        ::close( fd );
        return -1;
    }
    return fd;
}

inline int accept_tcp_client( int lfd, const std::atomic<bool>& quit, int sndbuf_bytes, int sndtimeo_sec )
{
    pollfd pf {};
    pf.fd = lfd;
    pf.events = POLLIN;
    while ( !quit.load() && ::poll( &pf, 1, 200 ) <= 0 )
    {}
    if ( quit.load() )
        return -1;

    const int cfd = ::accept4( lfd, nullptr, nullptr, SOCK_CLOEXEC );
    if ( cfd < 0 )
        return -1;

    int one = 1;
    ::setsockopt( cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
    if ( sndbuf_bytes > 0 )
        ::setsockopt( cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes, sizeof( sndbuf_bytes ) );
    if ( sndtimeo_sec > 0 )
    {
        const timeval tv { sndtimeo_sec, 0 };
        ::setsockopt( cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
    }
    return cfd;
}
