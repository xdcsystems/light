// TcpPullServer + tcp_send_chunk (include/platform/tcp_pull_server.hpp,
// src/platform/tcp_pull_server.cpp, src/platform/tcp_send.cpp) under
// ThreadSanitizer: concurrent abort_client() during tcp_send_chunk() and
// accept_client() on loopback sockets.

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "check.hpp"
#include "core/status.hpp"
#include "platform/tcp_pull_server.hpp"
#include "suites.hpp"

namespace
{
    using namespace std::chrono_literals;

    int reserve_ephemeral_port()
    {
        const int fd = ::socket( AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 );
        if ( fd < 0 )
            return 0;
        int one = 1;
        ::setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl( INADDR_ANY );
        addr.sin_port = 0;
        socklen_t len = sizeof( addr );
        int port = 0;
        if ( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) == 0 &&
             ::getsockname( fd, reinterpret_cast<sockaddr*>( &addr ), &len ) == 0 )
        {
            port = ntohs( addr.sin_port );
        }
        ::close( fd );
        return port;
    }

    int connect_loopback( int port )
    {
        const int fd = ::socket( AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 );
        if ( fd < 0 )
            return -1;
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        addr.sin_port = htons( static_cast<uint16_t>( port ) );
        if ( ::connect( fd, reinterpret_cast<const sockaddr*>( &addr ), sizeof( addr ) ) != 0 )
        {
            ::close( fd );
            return -1;
        }
        return fd;
    }

    int bind_server( std::optional<TcpPullServer>& srv, int sndbuf, int sndtimeo )
    {
        for ( int attempt = 0; attempt < 8; ++attempt )
        {
            const int port = reserve_ephemeral_port();
            if ( port <= 0 )
                continue;
            srv.emplace( port, sndbuf, sndtimeo );
            if ( srv->listen().ok() )
                return port;
            srv.reset();
        }
        return 0;
    }

    void check_abort_during_send( TestRun& t )
    {
        std::optional<TcpPullServer> srv;
        const int port = bind_server( srv, 32768, 5 );
        CHECK( t, port > 0 && srv.has_value() );
        if ( port <= 0 || !srv.has_value() )
            return;

        const std::vector<uint8_t> payload( 8u << 20, 0x5Au );
        const std::atomic<bool> quit { false };
        std::atomic<bool> hold_client { true };
        Status accept_st;
        int send_rc = 0;

        std::thread client( [port, &hold_client] {
            const int cfd = connect_loopback( port );
            while ( hold_client.load( std::memory_order_acquire ) )
                std::this_thread::sleep_for( 2ms );
            if ( cfd >= 0 )
                ::close( cfd );
        } );

        std::thread server( [&srv, &quit, &payload, &accept_st, &send_rc] {
            accept_st = srv->accept_client( quit );
            if ( accept_st.ok() )
                send_rc = tcp_send_chunk( srv->write_ctx(), payload.data(), static_cast<int>( payload.size() ) );
        } );

        const auto t0 = std::chrono::steady_clock::now();
        const auto deadline = t0 + 1500ms;
        while ( std::chrono::steady_clock::now() < deadline )
        {
            if ( srv->has_client() && srv->wait_writable_cnt() > 0 )
                break;
            std::this_thread::sleep_for( 1ms );
        }
        srv->abort_client();
        server.join();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0 )
                                    .count();

        hold_client.store( false, std::memory_order_release );
        client.join();
        srv->close_all();

        CHECK( t, accept_st.ok() );
        CHECK( t, send_rc < 0 );
        CHECK( t, elapsed_ms < 2500 );
    }

    void check_abort_during_accept( TestRun& t )
    {
        for ( int round = 0; round < 16; ++round )
        {
            std::optional<TcpPullServer> srv;
            const int port = bind_server( srv, 32768, 2 );
            CHECK( t, port > 0 && srv.has_value() );
            if ( port <= 0 || !srv.has_value() )
                return;

            std::atomic<bool> quit { false };
            std::atomic<bool> done { false };
            Status accept_st;

            std::thread server( [&srv, &quit, &done, &accept_st] {
                accept_st = srv->accept_client( quit );
                done.store( true, std::memory_order_release );
            } );

            std::thread client( [port, round, &done] {
                if ( ( round % 2 ) != 0 )
                    std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
                const int cfd = connect_loopback( port );
                while ( !done.load( std::memory_order_acquire ) )
                    std::this_thread::sleep_for( 1ms );
                if ( cfd >= 0 )
                    ::close( cfd );
            } );

            std::thread aborter( [&srv, &quit, round] {
                std::this_thread::sleep_for( std::chrono::microseconds( ( round % 4 ) * 50 ) );
                srv->abort_client();
                quit.store( true, std::memory_order_release );
            } );

            aborter.join();
            server.join();
            client.join();
            srv->close_all();

            CHECK( t, accept_st.ok() || accept_st.code() == Errc::Timeout );
        }
    }
}  // namespace

void test_tcp_pull_threads( TestRun& t )
{
    check_abort_during_send( t );
    check_abort_during_accept( t );
}
