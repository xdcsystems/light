// tcp_send_chunk: the drain-the-chunk send loop of TcpPullServer's AVIO
// write callback, POSIX only (no FFmpeg): host-tested in test_tcp_send.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "platform/tcp_pull_server.hpp"

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

    int wait_writable( int fd, bool has_to, std::chrono::steady_clock::time_point deadline ) noexcept
    {
        pollfd pf { fd, POLLOUT, 0 };
        int pr = 0;
        do
        {
            int timeout_ms = -1;
            if ( has_to )
            {
                const auto rem_ns = deadline - std::chrono::steady_clock::now();
                if ( rem_ns <= std::chrono::steady_clock::duration::zero() )
                    return -ETIMEDOUT;
                const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>( rem_ns ).count();
                timeout_ms = static_cast<int>( std::clamp<int64_t>( rem_ms, 1, INT_MAX ) );
            }
            pf.revents = 0;
            pr = ::poll( &pf, 1, timeout_ms );
        } while ( pr < 0 && errno == EINTR );
        if ( pr > 0 )
        {
            if ( ( pf.revents & POLLNVAL ) != 0 )
                return -EIO;
            if ( ( pf.revents & ( POLLERR | POLLHUP ) ) != 0 )
                return -EPIPE;
            return 0;
        }
        return pr == 0 ? -ETIMEDOUT : -EIO;
    }

    int handle_send_error( const TcpPullServer::WriteCtx& ctx, int fd, int err, bool has_to,
                           std::chrono::steady_clock::time_point deadline ) noexcept
    {
        if ( err == EINTR )
            return 0;
        if ( err == EAGAIN || err == EWOULDBLOCK )
        {
            ctx.wait_writable_cnt.fetch_add( 1, std::memory_order_relaxed );
            return wait_writable( fd, has_to, deadline );
        }
        return send_errno_code( err );
    }
}  // namespace

int tcp_send_chunk( const TcpPullServer::WriteCtx& ctx, const uint8_t* buf, int buf_size ) noexcept
{
    if ( buf_size <= 0 )
        return buf_size == 0 ? 0 : -EINVAL;
    const int fd = ctx.cfd.load( std::memory_order_acquire );
    if ( fd < 0 )
        return -EPIPE;
    // Drain-the-chunk send on a SOCK_NONBLOCK client socket: a partial return
    // here is silent data loss -- flush_buffer (libavformat/aviobuf.c) checks
    // only < 0 and resets buf_ptr = buf_ptr_max = buffer, the unsent tail is
    // never retried. On EAGAIN/EWOULDBLOCK, wait_writable() waits for POLLOUT
    // under a single chunk-wide deadline and counts TCP backpressure in ww.
    const bool has_to = ctx.sndtimeo_sec > 0;
    const auto deadline = has_to ? ( std::chrono::steady_clock::now() + std::chrono::seconds( ctx.sndtimeo_sec ) )
                                 : std::chrono::steady_clock::time_point {};
    size_t sent = 0;
    while ( sent < static_cast<size_t>( buf_size ) )
    {
        const ssize_t n = ::send( fd, buf + sent, static_cast<size_t>( buf_size ) - sent, MSG_NOSIGNAL );
        if ( n < 0 )
        {
            if ( const int rc = handle_send_error( ctx, fd, errno, has_to, deadline ); rc < 0 )
                return rc;
            continue;
        }
        if ( n == 0 )
            return kTcpSendEof;  // errno is stale after a zero return: never read it here
        sent += static_cast<size_t>( n );
        if ( sent < static_cast<size_t>( buf_size ) && has_to && std::chrono::steady_clock::now() >= deadline )
            return -ETIMEDOUT;
    }
    return static_cast<int>( sent );
}
