// NetMjpegSource end to end against a loopback server serving the fixtures
// (tests/data/): the real socket, parser, pool and source code, no fakes.
// The server side runs on a helper thread only while setup_device() reads
// the head (it must answer the GET); later bytes are written by the test
// thread itself before the dequeue that reads them.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "loopback.hpp"
#include "mjpeg_stream.hpp"
#include "net_mjpeg_source.hpp"
#include "suites.hpp"

using loopback::Listener;
using mjpeg_test::Bytes;

namespace
{
    const char* const kHead = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=frame\r\n\r\n";

    std::string str( const Bytes& b )
    {
        return std::string( b.begin(), b.end() );
    }

    // Parts with Content-Length; lead: a line break first (a continuation
    // after a previous Content-Length body, whose delimiter needs a line start).
    std::string parts( const std::vector<Bytes>& bodies, bool lead )
    {
        return ( lead ? "\r\n" : "" ) + str( mjpeg_test::make_stream( "--frame", bodies, true, "\r\n", false ) );
    }

    // The same geometry, other scan bytes: a different frame that still passes the check.
    Bytes variant( const Bytes& a, uint8_t v )
    {
        Bytes b = a;
        b[b.size() - 10] = v;
        return b;
    }

    class Rig
    {
    public:
        Rig() : log_( out, err ), src( log_ ) {}
        ~Rig() { drop_server(); }
        Rig( const Rig& ) = delete;
        Rig& operator=( const Rig& ) = delete;

        std::string url() const { return "http://127.0.0.1:" + std::to_string( l_.port() ) + "/video"; }

        // setup_device() on this thread while the helper accepts, reads the
        // GET and sends initial; the previous server end is closed first.
        int open( const std::string& initial, std::string& why )
        {
            drop_server();
            std::thread th(
                [this, &initial]
                {
                    srv_ = l_.accept();
                    request = loopback::read_request( srv_ );
                    loopback::write_all( srv_, initial );
                } );
            const Status st = src.setup_device( url() );
            th.join();
            why = st.what();
            return st.ok() ? 0 : -1;
        }
        void send( const std::string& s ) const { loopback::write_all( srv_, s ); }
        void drop_server()
        {
            if ( srv_ >= 0 )
                ::close( srv_ );
            srv_ = -1;
        }
        // One dequeue; frame = its bytes when it returned a slot.
        int dequeue( int timeout_ms, int& err, Bytes& frame )
        {
            uint32_t n = 0;
            int idx = -1;
            const Status st = src.dequeue( idx, n, timeout_ms );
            err = st.ok() ? 0 : st.sys();
            if ( !st.ok() )
                idx = -1;
            frame.clear();
            if ( idx >= 0 )
            {
                const auto* p = static_cast<const uint8_t*>( src.buffer( idx ) );
                frame.assign( p, p + n );
            }
            return idx;
        }

    public:
        std::ostringstream out;
        std::ostringstream err;
        std::string request;

    private:
        Listener l_;
        int srv_ = -1;
        Log log_;

    public:
        NetMjpegSource src;
    };

    void connect_and_frames( TestRun& t, const Bytes& a )
    {
        const Bytes v1 = variant( a, 0x11 );
        const Bytes v2 = variant( a, 0x22 );
        const Bytes v3 = variant( a, 0x33 );
        Rig r;
        std::string why;
        // Connect + prime, then start() drains the backlog to the freshest frame.
        CHECK( t, r.open( kHead + parts( { a, v1, v2 }, false ), why ) == 0 );
        CHECK( t, r.request.compare( 0, 21, "GET /video HTTP/1.1\r\n" ) == 0 );
        CHECK( t, r.src.width() == 16 && r.src.height() == 16 );
        const std::string connected = "net-src: connected " + r.url() + " (16x16), boundary frame\n";
        CHECK( t, r.out.str() == connected );
        r.src.start();
        int err = -1;
        Bytes f;
        int idx = r.dequeue( 1000, err, f );
        CHECK( t, idx == 0 && err == 0 && f == v2 );
        r.src.requeue( idx );
        // Nothing sent: EAGAIN within the wait.
        CHECK( t, r.dequeue( 50, err, f ) == -1 && err == EAGAIN );
        // Several parts queued: dequeue hands out the latest.
        r.send( parts( { v1, a, v3 }, true ) );
        idx = r.dequeue( 1000, err, f );
        CHECK( t, idx >= 0 && f == v3 );
        r.src.requeue( idx );
        // Blocking mode (timeout < 0) reads the next part.
        r.send( parts( { v1 }, true ) );
        idx = r.dequeue( -1, err, f );
        CHECK( t, idx >= 0 && f == v1 );
        r.src.requeue( idx );
        // EOF: the recoverable stall. Then teardown: ENODEV.
        r.drop_server();
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EIO );
        r.src.teardown();
        CHECK( t, r.dequeue( 0, err, f ) == -1 && err == ENODEV );
        CHECK( t, r.err.str().empty() );
    }

    void corrupt_streak( TestRun& t, const Bytes& a )
    {
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        int err = -1;
        Bytes f;
        r.src.requeue( r.dequeue( 1000, err, f ) );
        const Bytes no_eoi( a.begin(), a.end() - 2 );
        // A broken JPEG is dropped with EAGAIN, the streak counted; the 4th is EIO.
        for ( int i = 1; i <= 4; ++i )
        {
            r.send( parts( { no_eoi }, true ) );
            CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == ( i < 4 ? EAGAIN : EIO ) );
            const std::string text = "net-src: corrupt/incomplete JPEG frame (" + std::to_string( no_eoi.size() )
                                     + " bytes, streak " + std::to_string( i ) + ") dropped\n";
            CHECK( t, r.err.str().find( text ) != std::string::npos );
        }
        // A full streak: EIO at once, even with a good frame waiting.
        r.send( parts( { a }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EIO );
        // A reconnect (the recovery ladder's setup_device) clears it.
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        const int idx = r.dequeue( 1000, err, f );
        CHECK( t, idx == 0 && f == a );
        r.src.requeue( idx );
        // A good frame after one broken one resets the streak.
        r.send( parts( { no_eoi }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EAGAIN );
        r.send( parts( { a }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) >= 0 && f == a );
    }

    void desync_recovers( TestRun& t, const Bytes& a )
    {
        // Garbage past the 256 KiB window without a delimiter: the window is
        // dropped (one streak count, one resync retry), the next part is read.
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        int err = -1;
        Bytes f;
        r.src.requeue( r.dequeue( 1000, err, f ) );
        const Bytes v1 = variant( a, 0x44 );
        const std::string junk = "\r\n" + std::string( 300 * 1024, 'g' ) + parts( { v1 }, true );
        std::thread writer( [&r, &junk] { r.send( junk ); } );
        int idx = -1;
        bool eio = false;
        for ( int i = 0; i < 20 && idx < 0 && !eio; ++i )
        {
            idx = r.dequeue( 500, err, f );
            eio = idx < 0 && err != EAGAIN;
        }
        writer.join();
        CHECK( t, idx >= 0 && !eio && f == v1 );
    }

    void refused( TestRun& t, const Bytes& a, const Bytes& wide )
    {
        std::string why;
        // Another resolution on reconnect: refused, the decoder keeps its size.
        {
            Rig r;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            CHECK( t, r.open( kHead + parts( { wide }, false ), why ) == -1 && why == "stream resolution changed on reconnect" );
        }
        // The head's errors come through with their texts.
        {
            Rig r;
            CHECK( t, r.open( "HTTP/1.1 404 Not Found\r\n\r\n", why ) == -1 && why == "server did not answer 200" );
            CHECK( t, r.open( "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n\r\n", why ) == -1
                          && why == "no multipart boundary in Content-Type" );
            CHECK( t, r.open( kHead + std::string( "--frame\r\n\r\nnot a jpeg\r\n--frame\r\n" ), why ) == -1
                          && why == "initial JPEG frame is incomplete or missing SOFn" );
        }
        // Nobody listening; a bad URL.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            NetMjpegSource src( log );
            int port = 0;
            {
                const Listener gone;
                port = gone.port();
            }
            CHECK( t, [&]
            {
                const Status st = src.setup_device( "http://127.0.0.1:" + std::to_string( port ) + "/" );
                return !st.ok() && st.kind() == Status::Sys::Errno;  // the errno of the failed connect
            }() );
            CHECK( t, [&]
            {
                const Status st = src.setup_device( "rtsp://cam/" );
                return !st.ok() && st.code() == Errc::Invalid && std::strcmp( st.what(), "url must start with http://" ) == 0;
            }() );
            CHECK( t, [&]
            {
                int idx = -1;
                uint32_t n2 = 0;
                const Status st = src.dequeue( idx, n2, 0 );
                return !st.ok() && st.code() == Errc::Lost && st.sys() == ENODEV;
            }() );
        }
    }
}  // namespace

void test_net_mjpeg_source( TestRun& t )
{
    const Bytes a = mjpeg_test::load_fixture( "tiny_16x16.jpg" );
    const Bytes wide = mjpeg_test::load_fixture( "tiny_32x16.jpg" );
    CHECK( t, a.size() > 100 && wide.size() > 100 );
    if ( a.size() <= 100 || wide.size() <= 100 )
        return;
    connect_and_frames( t, a );
    corrupt_streak( t, a );
    desync_recovers( t, a );
    refused( t, a, wide );
}
