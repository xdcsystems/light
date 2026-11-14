#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.hpp"
#include "source/buffered_socket.hpp"
#include "suites.hpp"

using Fill = BufferedSocket::Fill;

namespace
{
    // A loopback listener on an ephemeral port; accept() hands out the server end.
    class Listener
    {
    public:
        Listener()
        {
            fd_ = ::socket( AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 );
            sockaddr_in a {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
            a.sin_port = 0;
            ::bind( fd_, reinterpret_cast<sockaddr*>( &a ), sizeof( a ) );
            ::listen( fd_, 4 );
            socklen_t len = sizeof( a );
            ::getsockname( fd_, reinterpret_cast<sockaddr*>( &a ), &len );
            port_ = ntohs( a.sin_port );
        }
        ~Listener()
        {
            if ( fd_ >= 0 )
                ::close( fd_ );
        }
        Listener( const Listener& ) = delete;
        Listener& operator=( const Listener& ) = delete;

        int port() const { return port_; }
        int accept() const { return ::accept( fd_, nullptr, nullptr ); }
        void close()
        {
            ::close( fd_ );
            fd_ = -1;
        }

    private:
        int fd_ = -1;
        int port_ = 0;
    };

    void write_all( int fd, const std::string& s )
    {
        size_t off = 0;
        while ( off < s.size() )
        {
            const ssize_t w = ::send( fd, s.data() + off, s.size() - off, MSG_NOSIGNAL );
            if ( w <= 0 )
                return;
            off += static_cast<size_t>( w );
        }
    }

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
        std::string why;
        CHECK( t, !s.is_open() && s.fill( 0 ) == Fill::Closed );
        CHECK( t, s.connect( "127.0.0.1", l.port(), &why ) == 0 && s.is_open() );
        const int srv = l.accept();
        CHECK( t, srv >= 0 );
        // send_all: the server gets every byte.
        const std::string req = "GET /video HTTP/1.1\r\n\r\n";
        CHECK( t, s.send_all( req, &why ) == 0 );
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
        CHECK( t, s.send_all( "x", &why ) == -1 && why.compare( 0, 6, "send: " ) == 0 );
    }

    void full_window( TestRun& t )
    {
        // More than the window, nothing consumed: Full, and nothing is read past it.
        Listener l;
        BufferedSocket s;
        CHECK( t, s.connect( "127.0.0.1", l.port(), nullptr ) == 0 );
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
        std::string why;
        // A port nobody listens on (a listener that was closed).
        int port = 0;
        {
            Listener l;
            port = l.port();
        }
        CHECK( t, s.connect( "127.0.0.1", port, &why ) == -1 && !s.is_open() );
        CHECK( t, why.compare( 0, 9, "connect: " ) == 0 );
        CHECK( t, s.connect( "", 80, &why ) == -1 && why == "addr resolution failed for " );
        // A failed connect closes what was open before.
        Listener l;
        CHECK( t, s.connect( "127.0.0.1", l.port(), nullptr ) == 0 );
        CHECK( t, s.connect( "127.0.0.1", port, nullptr ) == -1 && !s.is_open() );
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
        CHECK( t, s.connect( "127.0.0.1", l.port(), nullptr ) == 0 );
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
}  // namespace

void test_buffered_socket( TestRun& t )
{
    connect_and_receive( t );
    full_window( t );
    connect_failures( t );
    interrupted( t );
}
