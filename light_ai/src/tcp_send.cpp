// tcp_send_chunk: the drain-the-chunk send loop of TcpPullServer's AVIO
// write callback, POSIX only (no FFmpeg): host-tested in test_tcp_send.

#include <cerrno>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "tcp_pull_server.hpp"

namespace
{
    int send_errno_code( int err ) noexcept
    {
        if ( err == EPIPE || err == ECONNRESET || err == ESHUTDOWN || err == ENOTCONN )
            return -EPIPE;
        if ( err == EAGAIN || err == EWOULDBLOCK )
            return -ETIMEDOUT;
        return -EIO;
    }

    int wait_writable( int fd, std::chrono::steady_clock::time_point deadline ) noexcept
    {
        for ( ;; )
        {
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now() ).count();
            if ( rem <= 0 )
                return -ETIMEDOUT;
            pollfd pf { fd, POLLOUT, 0 };
            const int pr = ::poll( &pf, 1, static_cast<int>( rem ) );
            if ( pr > 0 )
                return 0;
            if ( pr == 0 )
                return -ETIMEDOUT;
            if ( errno != EINTR )
                return -EIO;
        }
    }
}  // namespace

int tcp_send_chunk( const TcpPullServer::WriteCtx& ctx, const uint8_t* buf, int buf_size ) noexcept
{
    const int fd = ctx.cfd.load( std::memory_order_acquire );
    if ( fd < 0 )
        return -EPIPE;
    // Drain-the-chunk send: a partial return here is silent data loss --
    // flush_buffer (libavformat/aviobuf.c) checks only < 0 and resets
    // buf_ptr = buf_ptr_max = buffer, the unsent tail is never retried.
    // A partial send at SO_SNDTIMEO expiry returns n > 0 rather than -1,
    // so retries enforce a single chunk-wide deadline via wait_writable().
    const bool has_to = ctx.sndtimeo_sec > 0;
    const auto deadline = has_to ? ( std::chrono::steady_clock::now() + std::chrono::seconds( ctx.sndtimeo_sec ) )
                                 : std::chrono::steady_clock::time_point {};
    size_t sent = 0;
    while ( sent < static_cast<size_t>( buf_size ) )
    {
        if ( sent > 0 && has_to )
        {
            ctx.wait_writable_cnt.fetch_add( 1, std::memory_order_relaxed );
            if ( const int wr = wait_writable( fd, deadline ); wr < 0 )
                return wr;
        }
        const ssize_t n = ::send( fd, buf + sent, static_cast<size_t>( buf_size ) - sent, MSG_NOSIGNAL );
        if ( n < 0 )
        {
            if ( errno == EINTR )
                continue;
            return send_errno_code( errno );
        }
        if ( n == 0 )
            return kTcpSendEof;  // errno is stale after a zero return: never read it here
        sent += static_cast<size_t>( n );
    }
    return static_cast<int>( sent );
}
