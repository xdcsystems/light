// SourceProber over SourceChannel + fakes: one round attaches only the non-Live
// channels among the first n, the busy-node list reaches the locator, the
// "absent" line is throttled, stop() returns within one 100 ms quantum.
// test_source_prober_threads: the real prober thread (1 ms cadence) against a
// frame thread that detaches -- 1000+ round trips; it also runs under TSan
// (light_ai_tests_tsan).

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/log.hpp"
#include "source/source_channel.hpp"
#include "source/source_prober.hpp"
#include "source/usb_node_locator.hpp"
#include "source_fakes.hpp"
#include "suites.hpp"

namespace
{
    using Channel = BasicSourceChannel<FakeSrc, FakeDec>;
    using Locator = BasicUsbNodeLocator<FakeHost>;
    using clk = std::chrono::steady_clock;

    enum
    {
        kChannels = 3,
        kRoundTrips = 1000,  // threaded suite: detach -> re-attach cycles, all channels together
        kDeadlineS = 30,
        kStopBudgetMs = 100,  // one quantum of the old probe_loop sleep
    };
    using Prober = BasicSourceProber<Channel, kChannels, Locator>;

    // Three V4L2 channels on /dev/video20..22, all present, logs captured.
    struct Rig
    {
        Rig()
          : log( out, err )
          , loc( host, log )
          , ch { { Channel( log, dev[0], dec[0] ), Channel( log, dev[1], dec[1] ), Channel( log, dev[2], dec[2] ) } }
        {
            host.present = { "/dev/video20", "/dev/video21", "/dev/video22" };
            host.usb = { 20, 21, 22 };
        }
        void configure( int s, SourceKind kind, const std::string& uri )
        {
            ch[static_cast<size_t>( s )].configure( s, { kind, uri }, 640, 640 );
        }
        void configure_all()
        {
            for ( int s = 0; s < kChannels; ++s )
                configure( s, SourceKind::V4L2, "/dev/video2" + std::to_string( s ) );
        }
        bool all_live( int n ) const
        {
            for ( int s = 0; s < n; ++s )
            {
                if ( !ch[static_cast<size_t>( s )].live() )
                    return false;
            }
            return true;
        }

    public:
        std::ostringstream out;
        std::ostringstream err;
        const Log log;
        FakeHost host;
        std::array<FakeDevice, kChannels> dev {};
        std::array<FakeDecoderCtl, kChannels> dec {};
        Locator loc;
        std::array<Channel, kChannels> ch;
    };

    int count_of( const std::string& hay, const std::string& needle )
    {
        int n = 0;
        for ( size_t p = hay.find( needle ); p != std::string::npos; p = hay.find( needle, p + needle.size() ) )
            ++n;
        return n;
    }

    void round_attaches_non_live( TestRun& t )
    {
        Rig r;
        r.configure_all();
        std::string why;
        CHECK( t, r.ch[1].attach( {}, r.loc, false, why ) == Channel::Attach::Live );  // s1: Live before the prober
        Prober p( r.ch, kChannels, r.loc, r.log );
        p.round();
        CHECK( t, r.all_live( kChannels ) );
        CHECK( t, r.dev[0].opens == 1 && r.dev[1].opens == 1 && r.dev[2].opens == 1 );  // s1 not reopened
        CHECK( t, r.out.str().find( "probe: s1 absent" ) == std::string::npos );
        CHECK( t, r.out.str().find( "source s0: attached (hot)" ) != std::string::npos );  // hot attach texts
        p.round();
        CHECK( t, r.dev[0].opens == 1 && r.dev[1].opens == 1 && r.dev[2].opens == 1 );  // all Live: no attach
        CHECK( t, p.rounds() == 2 );
    }

    void round_probes_first_n( TestRun& t )
    {
        Rig r;
        r.configure_all();
        Prober p( r.ch, 2, r.loc, r.log );
        p.round();
        CHECK( t, r.all_live( 2 ) );
        CHECK( t, !r.ch[2].live() && r.dev[2].opens == 0 && r.dev[2].trials.empty() );
    }

    void failed_attach_retries_and_log_throttle( TestRun& t )
    {
        Rig r;
        r.configure_all();
        r.dev[0].open_rc = -1;
        Prober p( r.ch, 1, r.loc, r.log );
        for ( int k = 0; k < 15; ++k )
            p.round();
        CHECK( t, !r.ch[0].live() && r.dev[0].opens == 15 );  // one attach per round, never throws
        CHECK( t, count_of( r.out.str(), "probe: s0 absent, attempt " ) == 2 );
        CHECK( t, r.out.str().find( "probe: s0 absent, attempt 1\n" ) != std::string::npos );
        CHECK( t, r.out.str().find( "probe: s0 absent, attempt 15\n" ) != std::string::npos );
        r.dev[0].open_rc = 0;
        p.round();
        CHECK( t, r.ch[0].live() && r.dev[0].opens == 16 );
        CHECK( t, r.dec[0].inits == 1 );
    }

    void busy_nodes( TestRun& t )
    {
        Rig r;
        r.configure( 0, SourceKind::V4L2, "/dev/video20" );
        r.configure( 1, SourceKind::V4L2, "/dev/video21" );
        r.configure( 2, SourceKind::NetMjpeg, "http://cam:8080/video" );
        const NodeList b0 = busy_v4l2_nodes( r.ch, kChannels, 0 );
        CHECK( t, b0.size() == 1 && b0[0] == "/dev/video21" );  // self and the net source left out
        const NodeList b2 = busy_v4l2_nodes( r.ch, kChannels, 2 );
        CHECK( t, b2.size() == 2 && node_listed( b2, "/dev/video20" ) && node_listed( b2, "/dev/video21" ) );
        CHECK( t, busy_v4l2_nodes( r.ch, 1, 0 ).empty() );  // only the first n count

        // Through the prober: s0's node is gone, s1 (not Live) still owns video21 --
        // the scan must skip it and adopt video22.
        Rig q;
        q.configure( 0, SourceKind::V4L2, "/dev/video20" );
        q.configure( 1, SourceKind::V4L2, "/dev/video21" );
        q.host.present = { "/dev/video21", "/dev/video22" };
        q.host.usb = { 21, 22 };
        q.dev[1].open_rc = -1;
        Prober p( q.ch, 2, q.loc, q.log );
        p.round();
        CHECK( t, q.ch[0].live() && q.ch[0].node() == "/dev/video22" );
        CHECK( t, !node_listed( q.dev[0].trials, "/dev/video21" ) );
        CHECK( t, !q.ch[1].live() );
    }

    void start_stop( TestRun& t )
    {
        Rig r;
        r.configure_all();
        {
            Prober p( r.ch, kChannels, r.loc, r.log );  // production cadence: 2 s
            p.start();
            p.start();  // already started: no second thread
            const auto deadline = clk::now() + std::chrono::milliseconds( 1000 );
            while ( !r.all_live( kChannels ) && clk::now() < deadline )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            CHECK( t, r.all_live( kChannels ) );  // the first round runs at once
            const auto t0 = clk::now();
            p.stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( clk::now() - t0 ).count();
            CHECK( t, ms < kStopBudgetMs );  // between rounds: stop() wakes the wait at once
            CHECK( t, p.rounds() == 1 );
            p.stop();  // idempotent; the destructor stops again
        }
        Rig z;
        {
            Prober p( z.ch, 0, z.loc, z.log );  // no sources: no thread
            p.start();
            p.stop();
            CHECK( t, p.rounds() == 0 );
        }
    }
}  // namespace

void test_source_prober( TestRun& t )
{
    round_attaches_non_live( t );
    round_probes_first_n( t );
    failed_attach_retries_and_log_throttle( t );
    busy_nodes( t );
    start_stop( t );
}

void test_source_prober_threads( TestRun& t )
{
    Rig r;
    r.configure_all();
    int detaches = 0;
    long geom_sum = 0;
    {
        Prober p( r.ch, kChannels, r.loc, r.log, 1 );  // 1 ms cadence: many round trips
        p.start();
        const auto deadline = clk::now() + std::chrono::seconds( kDeadlineS );
        for ( int iter = 0; detaches < kRoundTrips && clk::now() < deadline; ++iter )
        {
            // Frame side: one step per Live channel; every third poll fails with EIO.
            for ( int s = 0; s < kChannels; ++s )
            {
                Channel& c = r.ch[static_cast<size_t>( s )];
                if ( !c.live() )
                    continue;
                FakeDevice& d = r.dev[static_cast<size_t>( s )];
                d.dq_buf = ( iter % 3 == 0 ) ? -1 : 0;
                d.dq_err = EIO;
                geom_sum += c.geom().w + c.geom().lb.h;
                c.stamp_ok( clk::now() );
                EncodedFrame f {};
                const FrameStatus st = c.poll( 0, f );
                if ( st == FrameStatus::Frame )
                    static_cast<void>( c.decode( f ) );
                else if ( st == FrameStatus::Detached )
                {
                    c.detach();
                    ++detaches;
                }
            }
            std::this_thread::yield();
        }
        p.stop();
        CHECK( t, p.rounds() > 0 );
    }
    CHECK( t, detaches >= kRoundTrips );
    CHECK( t, geom_sum > 0 );
    for ( const FakeDecoderCtl& d : r.dec )
        CHECK( t, d.inits == 1 );  // re-attaches never re-initialize the decoder
    int opens = 0;
    for ( const FakeDevice& d : r.dev )
        opens += d.opens;
    CHECK( t, opens >= detaches );  // every detach was followed by a re-attach (or one is pending)
}
