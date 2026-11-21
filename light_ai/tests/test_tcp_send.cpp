// tcp_send_chunk (src/platform/tcp_send.cpp): TcpPullServer's AVIO write loop over a
// real socket pair -- a whole chunk delivered, the peer gone, abort_client's
// shutdown, no client, and the chunk-wide sndtimeo deadline (Low §3).

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.hpp"
#include "suites.hpp"
#include "platform/tcp_pull_server.hpp"

namespace
{
    struct Pair
    {
        Pair()
        {
            if ( ::socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd ) != 0 )
            {
                fd[0] = fd[1] = -1;
                return;
            }
            // Match accept4(SOCK_NONBLOCK) on the writer side; keep reader fd[1] blocking.
            const int flags = ::fcntl( fd[0], F_GETFL, 0 );
            if ( flags >= 0 )
                static_cast<void>( ::fcntl( fd[0], F_SETFL, flags | O_NONBLOCK ) );
        }
        ~Pair()
        {
            for ( const int f : fd )
                if ( f >= 0 )
                    ::close( f );
        }
        Pair( const Pair& ) = delete;
        Pair& operator=( const Pair& ) = delete;

        int fd[2] { -1, -1 };
    };

    std::vector<uint8_t> pattern( size_t n )
    {
        std::vector<uint8_t> v( n );
        for ( size_t i = 0; i < n; ++i )
            v[i] = static_cast<uint8_t>( i * 13 + 7 );
        return v;
    }

    void check_tcp_pull_server_lifecycle( TestRun& t )
    {
        TcpPullServer srv( 0, 65536, 1 );
        const std::atomic<bool> quit_now { true };
        CHECK( t, !srv.has_client() );
        const Status no_lfd = srv.accept_client( quit_now );
        CHECK( t, !no_lfd.ok() && no_lfd.code() == Errc::Invalid );
        CHECK( t, srv.listen().ok() );
        const Status cancelled = srv.accept_client( quit_now );
        CHECK( t, !cancelled.ok() && cancelled.code() == Errc::Timeout );
        CHECK( t, !srv.has_client() );
        srv.abort_client();
        srv.close_all();
        CHECK( t, !srv.has_client() );
    }
    void check_backpressure_and_bounds( TestRun& t )
    {
        // Temporary backpressure: EAGAIN -> wait_writable() -> reader resumes -> whole chunk delivered.
        {
            const Pair p;
            TcpPullServer::WriteCtx ctx( 2 );
            ctx.cfd.store( p.fd[0] );
            const std::vector<uint8_t> data = pattern( 2 << 20 );
            std::vector<uint8_t> got;
            std::thread reader( [&p, &got, n = data.size()] {
                std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
                std::vector<uint8_t> buf( 65536 );
                while ( got.size() < n )
                {
                    const ssize_t r = ::read( p.fd[1], buf.data(), buf.size() );
                    if ( r <= 0 )
                        break;
                    got.insert( got.end(), buf.begin(), buf.begin() + r );
                }
            } );
            const int r = tcp_send_chunk( ctx, data.data(), static_cast<int>( data.size() ) );
            ::shutdown( p.fd[0], SHUT_WR );
            reader.join();
            CHECK( t, r == static_cast<int>( data.size() ) );
            CHECK( t, got == data );
            CHECK( t, ctx.wait_writable_cnt.load() >= 1 );
        }
        // Non-positive buf_size: zero is a no-op, negative returns -EINVAL.
        {
            const TcpPullServer::WriteCtx ctx( 2 );
            const uint8_t b[1] {};
            CHECK( t, tcp_send_chunk( ctx, b, 0 ) == 0 );
            CHECK( t, tcp_send_chunk( ctx, b, -1 ) == -EINVAL );
        }
    }
}  // namespace

void test_tcp_send( TestRun& t )
{
    // A 1 MiB chunk (far above the socket buffer) arrives whole and in order.
    {
        Pair p;
        CHECK( t, p.fd[0] >= 0 );
        TcpPullServer::WriteCtx ctx( 2 );
        ctx.cfd.store( p.fd[0] );
        const std::vector<uint8_t> data = pattern( 1 << 20 );
        std::vector<uint8_t> got;
        std::thread reader( [&p, &got, n = data.size()] {
            std::vector<uint8_t> buf( 65536 );
            while ( got.size() < n )
            {
                const ssize_t r = ::read( p.fd[1], buf.data(), buf.size() );
                if ( r <= 0 )
                    break;
                got.insert( got.end(), buf.begin(), buf.begin() + r );
            }
        } );
        const int r = tcp_send_chunk( ctx, data.data(), static_cast<int>( data.size() ) );
        ::shutdown( p.fd[0], SHUT_WR );  // a short send must not leave the reader blocked in read() forever
        reader.join();
        CHECK( t, r == static_cast<int>( data.size() ) );
        CHECK( t, got == data );
    }
    // No client: EPIPE without touching any fd.
    {
        const TcpPullServer::WriteCtx ctx( 2 );
        const uint8_t b[4] = { 1, 2, 3, 4 };
        CHECK( t, tcp_send_chunk( ctx, b, 4 ) == -EPIPE );
    }
    // The viewer dropped: EPIPE (MSG_NOSIGNAL: no SIGPIPE kills the test).
    {
        Pair p;
        TcpPullServer::WriteCtx ctx( 2 );
        ctx.cfd.store( p.fd[0] );
        ::close( p.fd[1] );
        p.fd[1] = -1;
        const uint8_t b[16] {};
        CHECK( t, tcp_send_chunk( ctx, b, 16 ) == -EPIPE );
    }
    // abort_client's shutdown( SHUT_RDWR ): the next chunk fails with EPIPE at once.
    {
        const Pair p;
        TcpPullServer::WriteCtx ctx( 2 );
        ctx.cfd.store( p.fd[0] );
        ::shutdown( p.fd[0], SHUT_RDWR );
        const uint8_t b[16] {};
        CHECK( t, tcp_send_chunk( ctx, b, 16 ) == -EPIPE );
    }
    // A stalled viewer (never reads): the chunk-wide deadline ends the send
    // with ETIMEDOUT after sndtimeo_sec, partial progress included.
    {
        const Pair p;
        TcpPullServer::WriteCtx ctx( 1 );
        ctx.cfd.store( p.fd[0] );
        const std::vector<uint8_t> data = pattern( 8 << 20 );
        const auto t0 = std::chrono::steady_clock::now();
        const int r = tcp_send_chunk( ctx, data.data(), static_cast<int>( data.size() ) );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        CHECK( t, r == -ETIMEDOUT );
        CHECK( t, el >= 900 && el < 3000 );
        CHECK( t, ctx.wait_writable_cnt.load() >= 1 );
    }
    check_backpressure_and_bounds( t );
    check_tcp_pull_server_lifecycle( t );
}
