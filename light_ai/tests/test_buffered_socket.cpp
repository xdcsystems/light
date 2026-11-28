#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.hpp"
#include "loopback.hpp"
#include "platform/buffered_socket.hpp"
#include "status_text.hpp"
#include "suites.hpp"

using Fill = BufferedSocket::Fill;
using loopback::Listener;
using loopback::write_all;

namespace
{
    // Fills until want bytes are buffered or a non-Data result.
    Fill fill_until( BufferedSocket& s, size_t want )
    {
        Fill r = Fill::Data;
        while ( s.size() < want && r == Fill::Data )
            r = s.fill( 1000 );
        return r;
    }

    void on_usr1( int )
    {
    }

    void connect_and_receive( TestRun& t )
    {
        Listener l;
        BufferedSocket s;
        CHECK( t, !s.is_open() && s.fill( 0 ) == Fill::Closed );
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() && s.is_open() );
        const int srv = l.accept();
        CHECK( t, srv >= 0 );
        // send_all: the server gets every byte.
        const std::string req = "GET /video HTTP/1.1\r\n\r\n";
        CHECK( t, s.send_all( req ).ok() );
        std::vector<char> got( req.size() );
        size_t n = 0;
        while ( n < got.size() )
        {
            const ssize_t r = ::recv( srv, got.data() + n, got.size() - n, 0 );
            if ( r <= 0 )
                break;
            n += static_cast<size_t>( r );
        }
        CHECK( t, std::string( got.begin(), got.end() ) == req );
        // Nothing sent: Timeout; then Data into the window; consume() / compaction.
        CHECK( t, s.fill( 50 ) == Fill::Timeout );
        write_all( srv, "hello world" );
        CHECK( t, fill_until( s, 11 ) == Fill::Data );
        CHECK( t, s.size() == 11 && std::memcmp( s.data(), "hello world", 11 ) == 0 );
        s.consume( 6 );
        write_all( srv, "!" );
        CHECK( t, fill_until( s, 6 ) == Fill::Data );
        CHECK( t, s.size() == 6 && std::memcmp( s.data(), "world!", 6 ) == 0 );
        // EOF: the peer closes -> Closed; the buffered bytes stay readable.
        ::close( srv );
        Fill r = Fill::Data;
        while ( r == Fill::Data )
            r = s.fill( 1000 );
        CHECK( t, r == Fill::Closed && s.size() == 6 );
        // close(): socket and window gone, idempotent; send on a closed socket fails.
        s.close();
        s.close();
        CHECK( t, !s.is_open() && s.size() == 0 && s.fill( 0 ) == Fill::Closed );
        const Status st = s.send_all( "x" );
        CHECK( t, st.code() == Errc::Failed && status_text( st ) == "send: not connected" );
    }

    void full_window( TestRun& t )
    {
        // More than the window, nothing consumed: Full, and nothing is read past it.
        Listener l;
        BufferedSocket s;
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        const int srv = l.accept();
        std::thread writer( [srv] { write_all( srv, std::string( BufferedSocket::kWindow + 4096, 'z' ) ); } );
        const Fill r = fill_until( s, BufferedSocket::kWindow + 1 );
        CHECK( t, r == Fill::Full && s.full() && s.size() == static_cast<size_t>( BufferedSocket::kWindow ) );
        s.consume( 4096 );  // room again: the rest arrives
        CHECK( t, fill_until( s, BufferedSocket::kWindow ) == Fill::Data );
        writer.join();
        CHECK( t, s.full() );
        ::close( srv );
    }

    void connect_failures( TestRun& t )
    {
        BufferedSocket s;
        // A port nobody listens on (a listener that was closed).
        int port = 0;
        {
            Listener l;
            port = l.port();
        }
        const Status refused = s.connect( "127.0.0.1", port );
        CHECK( t, !refused.ok() && !s.is_open() );
        CHECK( t, refused.code() == Errc::Absent && refused.kind() == Status::Sys::Errno && refused.sys() == ECONNREFUSED );
        CHECK( t, status_text( refused ) == std::string( "connect: " ) + std::strerror( ECONNREFUSED ) );
        const Status unresolved = s.connect( "", 80 );
        CHECK( t, unresolved.code() == Errc::Absent && status_text( unresolved ) == "addr resolution failed" );
        // A failed connect closes what was open before.
        Listener l;
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        CHECK( t, !s.connect( "127.0.0.1", port ).ok() && !s.is_open() );
    }

    void interrupted( TestRun& t )
    {
        // A signal without SA_RESTART during the poll: Interrupted, not Timeout.
        struct sigaction sa {};
        struct sigaction old {};
        sa.sa_handler = on_usr1;
        sigemptyset( &sa.sa_mask );
        sa.sa_flags = 0;
        sigaction( SIGUSR1, &sa, &old );
        Listener l;
        BufferedSocket s;
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        const int srv = l.accept();
        std::atomic<bool> done { false };
        const pthread_t self = pthread_self();
        std::thread kicker(
            [&done, self]
            {
                // Repeated: a signal that lands before poll() starts is lost.
                while ( !done.load() )
                {
                    pthread_kill( self, SIGUSR1 );
                    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
                }
            } );
        const Fill r = s.fill( 3000 );
        done.store( true );
        kicker.join();
        sigaction( SIGUSR1, &old, nullptr );
        CHECK( t, r == Fill::Interrupted && s.is_open() );
        ::close( srv );
    }

    void wake_fd_interrupts_fill( TestRun& t )
    {
        // The wake fd alone, no stop flag: level-triggered and never read by the socket.
        const int efd = ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        CHECK( t, efd >= 0 );
        Listener l;
        BufferedSocket s;
        s.set_wake_fd( efd );
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        const int srv = l.accept();
        CHECK( t, srv >= 0 );
        // Signalled during the wait: poll() returns at once.
        std::thread waker( [efd] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            const uint64_t one = 1;
            static_cast<void>( ::write( efd, &one, sizeof( one ) ) );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Fill during = s.fill( 5000 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        waker.join();
        CHECK( t, during == Fill::Interrupted && el < 1000 );
        // Signalled before the wait: no lost wakeup, and fill_until() leaves instead of spinning to its deadline.
        const auto t1 = std::chrono::steady_clock::now();
        const Fill before = s.fill( 5000 );
        const Fill bounded = s.fill_until( 2000, 50 );
        const auto el1 = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t1 ).count();
        CHECK( t, before == Fill::Interrupted && bounded == Fill::Interrupted && el1 < 200 );
        CHECK( t, s.is_open() && !s.cancelled() );
        // send_all() and connect() are cancelled by it too.
        const Status sent = s.send_all( "x" );
        CHECK( t, sent.code() == Errc::Timeout && status_text( sent ) == "send: cancelled" );
        const Status again = s.connect( "127.0.0.1", l.port() );
        CHECK( t, again.code() == Errc::Timeout && status_text( again ) == "connect: cancelled" && !s.is_open() );
        s.set_wake_fd( -1 );  // the socket drops its non-owning copy before the descriptor is closed
        ::close( efd );
        ::close( srv );
    }

    // Small kernel buffers: a peer that never reads is full after a few writes.
    void shrink_buffer( int fd, int option )
    {
        const int bytes = 4096;
        static_cast<void>( ::setsockopt( fd, SOL_SOCKET, option, &bytes, sizeof( bytes ) ) );
    }

    // Fills the buffers of both ends without blocking, until the kernel takes no more.
    bool fill_send_buffers( int fd )
    {
        const std::vector<char> chunk( 64 * 1024, 'f' );
        bool filled = false;
        for ( int i = 0; i < 512 && !filled; ++i )
        {
            const ssize_t r = ::send( fd, chunk.data(), chunk.size(), MSG_DONTWAIT | MSG_NOSIGNAL );
            filled = r < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK );
        }
        return filled;
    }

    // Writes the eventfd after delay_ms, from a thread joined by the destructor.
    class DelayedWake
    {
    public:
        DelayedWake( int efd, int delay_ms )
          : thread_( [efd, delay_ms] {
              std::this_thread::sleep_for( std::chrono::milliseconds( delay_ms ) );
              const uint64_t one = 1;
              static_cast<void>( ::write( efd, &one, sizeof( one ) ) );
          } )
        {
        }
        ~DelayedWake() { thread_.join(); }
        DelayedWake( const DelayedWake& ) = delete;
        DelayedWake& operator=( const DelayedWake& ) = delete;

    private:
        std::thread thread_;
    };

    long long ms_since( std::chrono::steady_clock::time_point t0 )
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
    }

    void wake_fd_ends_blocked_send( TestRun& t )
    {
        // The wake fd alone, signalled while send_all() waits in poll() for a full socket.
        const int efd = ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        CHECK( t, efd >= 0 );
        {
            Listener l;
            BufferedSocket s;
            s.set_wake_fd( efd );
            CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
            const int srv = l.accept();
            CHECK( t, srv >= 0 );
            shrink_buffer( s.fd(), SO_SNDBUF );
            shrink_buffer( srv, SO_RCVBUF );
            CHECK( t, fill_send_buffers( s.fd() ) );
            const auto t0 = std::chrono::steady_clock::now();
            const DelayedWake waker( efd, 150 );
            const Status st = s.send_all( std::string( 64 * 1024, 'a' ) );
            const long long el = ms_since( t0 );
            CHECK( t, st.code() == Errc::Timeout && status_text( st ) == "send: cancelled" );
            CHECK( t, el >= 100 && el < 1500 );
            ::close( srv );
        }
        ::close( efd );  // after the socket's destructor
    }

    void wake_fd_ends_blocked_connect( TestRun& t )
    {
        // A listener whose accept queue is full drops the next SYN, so connect() stays in poll() until woken.
        const int efd = ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        CHECK( t, efd >= 0 );
        {
            Listener l( 0 );  // backlog 0: one connection fits into the queue
            BufferedSocket first;
            CHECK( t, first.connect( "127.0.0.1", l.port() ).ok() );  // never accepted
            BufferedSocket s;
            s.set_wake_fd( efd );
            const auto t0 = std::chrono::steady_clock::now();
            const DelayedWake waker( efd, 150 );
            const Status st = s.connect( "127.0.0.1", l.port() );
            const long long el = ms_since( t0 );
            CHECK( t, st.code() == Errc::Timeout && status_text( st ) == "connect: cancelled" && !s.is_open() );
            CHECK( t, el >= 100 && el < 1500 );
        }
        ::close( efd );  // after the sockets' destructors
    }

    void cancel_flag( TestRun& t )
    {
        // Pre-cancelled connect fails immediately; cancel during fill_until wakes promptly.
        std::atomic<bool> stop { true };
        BufferedSocket s;
        s.set_cancel( &stop );
        const Status cst = s.connect( "127.0.0.1", 1 );
        CHECK( t, !cst.ok() && cst.code() == Errc::Timeout );

        stop.store( false );
        Listener l;
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        const int srv = l.accept();
        std::thread waker( [&stop] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 60 ) );
            stop.store( true );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Fill r = s.fill_until( 2000, 50 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        waker.join();
        CHECK( t, r == Fill::Interrupted && el < 1000 );
        ::close( srv );
    }

    void send_cancel( TestRun& t )
    {
        // A peer that never reads: send_all() must leave on cancel instead of blocking in send().
        std::atomic<bool> stop { false };
        Listener l;
        BufferedSocket s;
        s.set_cancel( &stop );
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() );
        const int srv = l.accept();
        CHECK( t, srv >= 0 );
        shrink_buffer( s.fd(), SO_SNDBUF );
        shrink_buffer( srv, SO_RCVBUF );
        CHECK( t, fill_send_buffers( s.fd() ) );
        std::thread waker( [&stop] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            stop.store( true, std::memory_order_release );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = s.send_all( std::string( 64 * 1024, 'a' ) );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        waker.join();
        CHECK( t, el < 1500 );
        CHECK( t, st.code() == Errc::Timeout && status_text( st ) == "send: cancelled" );
        ::close( srv );
    }

    void hostname_lookup( TestRun& t )
    {
        // Stop already set: a hostname returns Timeout at once, no lookup started.
        std::atomic<bool> stop { true };
        BufferedSocket s;
        s.set_cancel( &stop );
        const auto t0 = std::chrono::steady_clock::now();
        const Status cst = s.connect( "nonexistent.invalid", 8080 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        CHECK( t, cst.code() == Errc::Timeout && !s.is_open() && el < 100 );
        // The prober's quit flag alone cancels too.
        std::atomic<bool> quit { true };
        stop.store( false );
        s.set_cancel( &stop, &quit );
        CHECK( t, s.connect( "nonexistent.invalid", 8080 ).code() == Errc::Timeout );
        // A hostname from /etc/hosts goes through getaddrinfo_a (no DNS) and connects;
        // the numeric host still takes the AI_NUMERICHOST path.
        quit.store( false );
        Listener l;
        CHECK( t, s.connect( "localhost", l.port() ).ok() && s.is_open() );
        const int srv = l.accept();
        CHECK( t, srv >= 0 );
        ::close( srv );
        CHECK( t, s.connect( "127.0.0.1", l.port() ).ok() && s.is_open() );
        const int srv2 = l.accept();
        CHECK( t, srv2 >= 0 );
        ::close( srv2 );
        // Refused after a hostname lookup: the connect error, not a lookup error.
        int port = 0;
        {
            Listener gone;
            port = gone.port();
        }
        const Status refused = s.connect( "localhost", port );
        CHECK( t, refused.code() == Errc::Absent && refused.sys() == ECONNREFUSED );
    }
}  // namespace

void test_buffered_socket( TestRun& t )
{
    connect_and_receive( t );
    full_window( t );
    connect_failures( t );
    interrupted( t );
    wake_fd_interrupts_fill( t );
    wake_fd_ends_blocked_send( t );
    wake_fd_ends_blocked_connect( t );
    cancel_flag( t );
    send_cancel( t );
    hostname_lookup( t );
}
