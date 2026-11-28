// NetMjpegSource end to end against a loopback server serving the fixtures
// (tests/data/): the real socket, parser, pool and source code, no fakes.
// The server side runs on a helper thread only while setup_device() reads
// the head (it must answer the GET); later bytes are written by the test
// thread itself before the dequeue that reads them.

#include <atomic>
#include <chrono>
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
#include "source/net_mjpeg_source.hpp"
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

    // Bad frames behind the delivered one in the same burst stay in the streak; a good one after them clears it.
    void burst_bad_tail( TestRun& t, const Bytes& a )
    {
        const Bytes no_eoi( a.begin(), a.end() - 2 );
        const Bytes v1 = variant( a, 0x66 );
        const Bytes v2 = variant( a, 0x77 );
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        int err = -1;
        Bytes f;
        r.src.requeue( r.dequeue( 1000, err, f ) );

        // { good, 4 x bad }: the good frame is delivered, the streak of 4 makes the next call EIO.
        r.send( parts( { v1, no_eoi, no_eoi, no_eoi, no_eoi }, true ) );
        int idx = r.dequeue( 1000, err, f );
        CHECK( t, idx >= 0 && err == 0 && f == v1 );
        r.src.requeue( idx );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EIO );

        // { bad, bad, good }: the good frame at the end clears the streak: 3 more bad ones are EAGAIN, the 4th EIO.
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        r.src.requeue( r.dequeue( 1000, err, f ) );
        r.send( parts( { no_eoi, no_eoi, v2 }, true ) );
        idx = r.dequeue( 1000, err, f );
        CHECK( t, idx >= 0 && err == 0 && f == v2 );
        r.src.requeue( idx );
        for ( int i = 1; i <= 4; ++i )
        {
            r.send( parts( { no_eoi }, true ) );
            CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == ( i < 4 ? EAGAIN : EIO ) );
        }

        // { good, 2 x bad }: a streak of 2, then 2 more bad calls reach 4.
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        r.src.requeue( r.dequeue( 1000, err, f ) );
        r.send( parts( { v1, no_eoi, no_eoi }, true ) );
        idx = r.dequeue( 1000, err, f );
        CHECK( t, idx >= 0 && f == v1 );
        r.src.requeue( idx );
        r.send( parts( { no_eoi }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EAGAIN );
        r.send( parts( { no_eoi }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EIO );

        // No good frame at all: every part of the burst counts, the first call is EIO.
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        r.src.start();
        r.src.requeue( r.dequeue( 1000, err, f ) );
        r.send( parts( { no_eoi, no_eoi, no_eoi, no_eoi }, true ) );
        CHECK( t, r.dequeue( 1000, err, f ) == -1 && err == EIO );
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

    void resize_and_refusals( TestRun& t, const Bytes& a, const Bytes& wide )
    {
        std::string why;
        // Another resolution on reconnect: adopted (SourceChannel re-initializes
        // the decoder), and its frames pass the geometry check.
        {
            Rig r;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            CHECK( t, r.open( kHead + parts( { wide, variant( wide, 0x55 ) }, false ), why ) == 0 );
            CHECK( t, r.src.width() == 32 && r.src.height() == 16 );
            r.src.start();
            int err = 0;
            Bytes f;
            CHECK( t, r.dequeue( 1000, err, f ) >= 0 && f == variant( wide, 0x55 ) );
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

    // Builds a valid JPEG > 256 KiB by inserting COM (FF FE) segments right after SOI (FF D8).
    Bytes make_large_jpeg( const Bytes& base, size_t min_bytes, uint8_t fill )
    {
        Bytes out;
        out.reserve( min_bytes + base.size() + 64 );
        out.push_back( base[0] );
        out.push_back( base[1] );
        size_t added = 0;
        while ( out.size() + base.size() - 2 < min_bytes )
        {
            const size_t payload = 60000;
            const uint16_t seg_len = static_cast<uint16_t>( payload + 2 );
            out.push_back( 0xFF );
            out.push_back( 0xFE );
            out.push_back( static_cast<uint8_t>( seg_len >> 8 ) );
            out.push_back( static_cast<uint8_t>( seg_len & 0xFF ) );
            out.insert( out.end(), payload, static_cast<uint8_t>( fill + ( added & 0x0F ) ) );
            added += payload;
        }
        out.insert( out.end(), base.begin() + 2, base.end() );
        return out;
    }

    int poll_nonblocking_frame( Rig& r, int timeout_ms, int& err, Bytes& f )
    {
        int idx = -1;
        for ( int i = 0; i < 40; ++i )
        {
            idx = r.dequeue( timeout_ms, err, f );
            if ( idx >= 0 || err != EAGAIN )
                break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        }
        return idx;
    }

    void large_jpeg_nonblocking( TestRun& t, const Bytes& a )
    {
        const Bytes big1 = make_large_jpeg( a, 380 * 1024, 0x31 );
        const Bytes big2 = make_large_jpeg( variant( a, 0x55 ), 520 * 1024, 0x62 );
        CHECK( t, big1.size() > 350 * 1024 && big2.size() > 500 * 1024 );

        // 1) With Content-Length: non-blocking dequeue (timeout_ms >= 0) and start() drain.
        {
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a, big1 }, false ), why ) == 0 );
            r.src.start();
            int err = -1;
            Bytes f;
            int idx = r.dequeue( 0, err, f );
            CHECK( t, idx == 0 && err == 0 && f == big1 );
            r.src.requeue( idx );

            const std::string stream2 = parts( { big2 }, true );
            std::thread writer( [&r, &stream2] { r.send( stream2 ); } );
            idx = poll_nonblocking_frame( r, 5, err, f );
            writer.join();
            CHECK( t, idx >= 0 && err == 0 && f == big2 );
            r.src.requeue( idx );
        }

        // 2) Without Content-Length (boundary-scan mode) in non-blocking mode.
        {
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            r.src.start();
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( 0, err, f ) );

            const std::string no_cl = "\r\n" + str( mjpeg_test::make_stream( "--frame", { big1, big2 }, false, "\r\n", false ) )
                                      + "\r\n--frame\r\n";
            std::thread writer( [&r, &no_cl] { r.send( no_cl ); } );
            int idx = poll_nonblocking_frame( r, 0, err, f );
            CHECK( t, idx >= 0 && err == 0 && f == big1 );
            r.src.requeue( idx );

            idx = poll_nonblocking_frame( r, 5, err, f );
            writer.join();
            CHECK( t, idx >= 0 && err == 0 && f == big2 );
            r.src.requeue( idx );
        }
    }

    void oversized_part_recovers( TestRun& t, const Bytes& a )
    {
        const Bytes huge = make_large_jpeg( variant( a, 0x41 ), 1250 * 1024, 0x21 );
        const Bytes good = make_large_jpeg( variant( a, 0x77 ), 380 * 1024, 0x51 );
        CHECK( t, huge.size() > ( 1u << 20 ) && good.size() > 350 * 1024 );

        // 1) Single > 1 MiB Content-Length part drains cleanly and recovers the next ~380 KiB frame.
        {
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            r.src.start();
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( 1000, err, f ) );

            const std::string stream = parts( { huge, good }, true );
            std::thread writer( [&r, &stream] { r.send( stream ); } );
            int idx = -1;
            for ( int i = 0; i < 60; ++i )
            {
                idx = r.dequeue( 5, err, f );
                if ( idx >= 0 || err != EAGAIN )
                    break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
            writer.join();
            CHECK( t, idx >= 0 && err == 0 && f == good );
            r.src.requeue( idx );
        }

        // 2) > 2 MiB boundary-scan part (no Content-Length) also drains cleanly and recovers.
        {
            const Bytes huge2 = make_large_jpeg( variant( a, 0x42 ), 2200 * 1024, 0x22 );
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            r.src.start();
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( 1000, err, f ) );

            const std::string stream = "\r\n" + str( mjpeg_test::make_stream( "--frame", { huge2, good }, false, "\r\n", false ) )
                                       + "\r\n--frame\r\n";
            std::thread writer( [&r, &stream] { r.send( stream ); } );
            int idx = -1;
            for ( int i = 0; i < 60; ++i )
            {
                idx = r.dequeue( 5, err, f );
                if ( idx >= 0 || err != EAGAIN )
                    break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
            writer.join();
            CHECK( t, idx >= 0 && err == 0 && f == good );
            r.src.requeue( idx );
        }

        // 3) 4 consecutive > 1 MiB parts still trip kErrStreakMax -> EIO.
        {
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            r.src.start();
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( 1000, err, f ) );

            const std::string stream = parts( { huge, huge, huge, huge }, true );
            std::thread writer( [&r, &stream] { r.send( stream ); } );
            int idx = -1;
            for ( int i = 0; i < 80; ++i )
            {
                idx = r.dequeue( 5, err, f );
                if ( idx >= 0 || err != EAGAIN )
                    break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
            writer.join();
            CHECK( t, idx == -1 && err == EIO );
        }
    }

    // Blocking dequeue: the retry after a bad part counts toward kErrStreakMax
    // too (regression: a bad retry was not counted, a Desync retry kept the
    // wedged window), so 4 bad parts in a row give EIO on the second call.
    void bad_retry_counts_blocking( TestRun& t, const Bytes& a )
    {
        // 1) 4 x 1.1 MiB Content-Length parts, then a good one: EAGAIN, then EIO.
        {
            const Bytes huge = make_large_jpeg( variant( a, 0x43 ), 1126 * 1024, 0x23 );
            CHECK( t, huge.size() > ( 1u << 20 ) );
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( -1, err, f ) );

            const std::string stream = parts( { huge, huge, huge, huge, variant( a, 0x45 ) }, true );
            std::thread writer( [&r, &stream] { r.send( stream ); } );
            const int idx1 = r.dequeue( -1, err, f );
            const int err1 = err;
            const int idx2 = r.dequeue( -1, err, f );
            writer.join();
            CHECK( t, idx1 == -1 && err1 == EAGAIN );
            CHECK( t, idx2 == -1 && err == EIO );
        }
        // 2) The retry returns Desync: 4 wedged windows of delimiter-less junk,
        // each dropped and counted -> EAGAIN, then EIO.
        {
            Rig r;
            std::string why;
            CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
            int err = -1;
            Bytes f;
            r.src.requeue( r.dequeue( -1, err, f ) );

            const std::string stream = "\r\n" + std::string( 4 * 256 * 1024 - 2, 'g' ) + parts( { variant( a, 0x46 ) }, true );
            std::thread writer( [&r, &stream] { r.send( stream ); } );
            const int idx1 = r.dequeue( -1, err, f );
            const int err1 = err;
            const int idx2 = r.dequeue( -1, err, f );
            writer.join();
            CHECK( t, idx1 == -1 && err1 == EAGAIN );
            CHECK( t, idx2 == -1 && err == EIO );
        }
    }

    void cancel_stops_blocking_read( TestRun& t, const Bytes& a )
    {
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        int idx = -1;
        uint32_t sz = 0;
        CHECK( t, r.src.dequeue( idx, sz, -1 ).ok() );
        r.src.requeue( idx );

        std::atomic<bool> stop { false };
        r.src.set_cancel( &stop );
        std::thread waker( [&stop] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 80 ) );
            stop.store( true );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = r.src.dequeue( idx, sz, -1 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        waker.join();
        CHECK( t, !st.ok() && st.code() == Errc::Lost && el < 1000 );
    }

    // M-15: a server that drips the response head a byte every 250 ms kept
    // resetting the 2 s per-fill watchdog; the wall-clock deadline ends the
    // setup with Timeout after kPollMs (2 s) whatever the drip rate.
    void slow_drip_head_times_out( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        NetMjpegSource src( log );
        Listener l;
        std::atomic<bool> stop { false };
        std::thread drip( [&l, &stop] {
            const int fd = l.accept();
            if ( fd < 0 )
                return;
            static_cast<void>( loopback::read_request( fd ) );
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nX-Pad: ";
            for ( size_t i = 0; !stop.load(); ++i )
            {
                const char c = i < head.size() ? head[i] : 'x';
                if ( ::send( fd, &c, 1, MSG_NOSIGNAL ) != 1 )
                    break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
            }
            ::close( fd );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = src.setup_device( "http://127.0.0.1:" + std::to_string( l.port() ) + "/video" );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        stop.store( true );
        drip.join();
        CHECK( t, !st.ok() && st.code() == Errc::Timeout );
        CHECK( t, el >= 1900 && el < 3500 );
    }

    // M-15 / #117: a part that has started and then drips (a byte every 250 ms,
    // each inside the 2 s per-fill watchdog) ends with Lost -- not never, and
    // not from the entry: the next delimiter line arrives first and sits in the
    // window through a 1.5 s idle gap, which counts toward no deadline.
    // drip_headers false: the headers come in one burst with 8 body bytes and
    // the body drips -> Lost kPartMs (5 s) after the first body byte;
    // drip_headers true: the header lines drip -> Lost kPollMs (2 s) after the
    // first header byte.
    void slow_drip_part_times_out( TestRun& t, const Bytes& a, bool drip_headers )
    {
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        int idx = -1;
        uint32_t sz = 0;
        CHECK( t, r.src.dequeue( idx, sz, -1 ).ok() );  // the primed first frame
        r.src.requeue( idx );

        const std::string next = parts( { a }, true );
        const size_t delim_end = next.find( "--frame" ) + std::string( "--frame\r\n" ).size();
        std::atomic<bool> stop { false };
        std::thread drip( [&r, &stop, &next, delim_end, drip_headers] {
            r.send( next.substr( 0, delim_end ) );  // the delimiter line only
            std::this_thread::sleep_for( std::chrono::milliseconds( 1500 ) );
            const size_t burst = drip_headers ? delim_end + 1 : next.find( "\r\n\r\n", delim_end ) + 4 + 8;
            r.send( next.substr( delim_end, burst - delim_end ) );
            for ( size_t i = burst; i + 1 < next.size() && !stop.load(); ++i )
            {
                r.send( next.substr( i, 1 ) );
                std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
            }
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = r.src.dequeue( idx, sz, -1 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        stop.store( true );
        drip.join();
        CHECK( t, !st.ok() && st.code() == Errc::Lost );
        if ( drip_headers )
            CHECK( t, el >= 3300 && el < 4800 );
        else
            CHECK( t, el >= 6300 && el < 8000 );
    }

    // #117: the part deadline starts at the first body byte. The next part's
    // header comes right after the frame; its body starts after a 1.5 s gap
    // (under kPollMs) and arrives in 5 pieces 1 s apart: 5.5 s from the header,
    // 4 s from the first body byte -> Part (it was Lost while kPartMs ran
    // from the first header byte).
    void body_deadline_from_first_byte( TestRun& t, const Bytes& a )
    {
        Rig r;
        std::string why;
        CHECK( t, r.open( kHead + parts( { a }, false ), why ) == 0 );
        int idx = -1;
        uint32_t sz = 0;
        CHECK( t, r.src.dequeue( idx, sz, -1 ).ok() );  // the primed first frame
        r.src.requeue( idx );

        const Bytes v = variant( a, 0x66 );
        const std::string next = parts( { v }, true );
        const size_t body_at = next.find( "\r\n\r\n", next.find( "--frame" ) ) + 4;
        std::thread feeder( [&r, &next, body_at] {
            r.send( next.substr( 0, body_at ) );  // the delimiter and the whole header
            const size_t piece = ( next.size() - body_at ) / 5;
            for ( size_t i = 0; i < 5; ++i )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( i == 0 ? 1500 : 1000 ) );
                const size_t from = body_at + i * piece;
                r.send( i < 4 ? next.substr( from, piece ) : next.substr( from ) );
            }
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = r.src.dequeue( idx, sz, -1 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - t0 ).count();
        feeder.join();
        CHECK( t, st.ok() && el >= 5300 && el < 7000 );
        if ( st.ok() )
        {
            const auto* p = static_cast<const uint8_t*>( r.src.buffer( idx ) );
            CHECK( t, Bytes( p, p + sz ) == v );
            r.src.requeue( idx );
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
    burst_bad_tail( t, a );
    desync_recovers( t, a );
    large_jpeg_nonblocking( t, a );
    oversized_part_recovers( t, a );
    bad_retry_counts_blocking( t, a );
    resize_and_refusals( t, a, wide );
    cancel_stops_blocking_read( t, a );
    slow_drip_head_times_out( t );
    slow_drip_part_times_out( t, a, false );
    slow_drip_part_times_out( t, a, true );
    body_deadline_from_first_byte( t, a );
}
