// SourceProber over a SourceSet of SourceChannels + fakes: one round attaches
// only the non-Live configured channels, the busy-node list reaches the
// locator, the "absent" line is throttled, stop() returns within one 100 ms quantum.
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
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "source/source_channel.hpp"
#include "source/source_prober.hpp"
#include "source/source_set.hpp"
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
        kStopBudgetMs = 100,  // one quantum of the old 3a prober sleep
    };
    using Set = BasicSourceSet<Channel, kChannels>;
    using Prober = BasicSourceProber<Set, Locator>;

    // Up to three channels; /dev/video20..22 present; logs captured.
    struct Rig
    {
        Rig()
          : log( out, err )
          , loc( host, log )
          , ch( log, [this]( int i ) { return Channel( log, dev[static_cast<size_t>( i )], dec[static_cast<size_t>( i )] ); } )
        {
            host.present = { "/dev/video20", "/dev/video21", "/dev/video22" };
            host.usb = { 20, 21, 22 };
        }
        // n V4L2 channels on /dev/video20.., in order.
        void configure_v4l2( int n )
        {
            std::vector<SourceEndpoint> eps;
            for ( int s = 0; s < n; ++s )
                eps.push_back( { SourceKind::V4L2, "/dev/video2" + std::to_string( s ) } );
            ch.configure( eps, 640, 640 );
        }
        void configure_all() { configure_v4l2( kChannels ); }
        bool all_live( int n ) const
        {
            for ( int s = 0; s < n; ++s )
            {
                if ( !ch[s].live() )
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
        Set ch;
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
        Prober p( r.ch, r.loc, r.log );
        p.round();
        CHECK( t, r.all_live( kChannels ) );
        CHECK( t, r.dev[0].opens == 1 && r.dev[1].opens == 1 && r.dev[2].opens == 1 );  // s1 not reopened
        CHECK( t, r.out.str().find( "probe: s1 absent" ) == std::string::npos );
        CHECK( t, r.out.str().find( "source s0: attached (hot)" ) != std::string::npos );  // hot attach texts
        p.round();
        CHECK( t, r.dev[0].opens == 1 && r.dev[1].opens == 1 && r.dev[2].opens == 1 );  // all Live: no attach
        CHECK( t, p.rounds() == 2 );
    }

    void round_probes_configured_only( TestRun& t )
    {
        Rig r;
        r.configure_v4l2( 2 );
        Prober p( r.ch, r.loc, r.log );
        p.round();
        CHECK( t, r.all_live( 2 ) );
        CHECK( t, !r.ch[2].live() && r.dev[2].opens == 0 && r.dev[2].trials.empty() );
    }

    void failed_attach_retries_and_log_throttle( TestRun& t )
    {
        Rig r;
        r.configure_v4l2( 1 );
        r.dev[0].open_rc = -1;
        Prober p( r.ch, r.loc, r.log );
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

    // s0's node is gone, s1 (not Live) still owns video21: the scan must skip
    // it and adopt video22 (the busy list comes from SourceSet::busy_nodes).
    void busy_nodes_reach_the_locator( TestRun& t )
    {
        Rig q;
        q.configure_v4l2( 2 );
        q.host.present = { "/dev/video21", "/dev/video22" };
        q.host.usb = { 21, 22 };
        q.dev[1].open_rc = -1;
        Prober p( q.ch, q.loc, q.log );
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
            Prober p( r.ch, r.loc, r.log );  // production cadence: 2 s
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
        z.configure_v4l2( 0 );
        {
            Prober p( z.ch, z.loc, z.log );  // no sources: no thread
            p.start();
            p.stop();
            CHECK( t, p.rounds() == 0 );
        }
    }
}  // namespace

void test_source_prober( TestRun& t )
{
    round_attaches_non_live( t );
    round_probes_configured_only( t );
    failed_attach_retries_and_log_throttle( t );
    busy_nodes_reach_the_locator( t );
    start_stop( t );
}

void test_source_prober_threads( TestRun& t )
{
    Rig r;
    r.configure_all();
    int detaches = 0;
    long geom_sum = 0;
    {
        Prober p( r.ch, r.loc, r.log, 1 );  // 1 ms cadence: many round trips
        p.start();
        const auto deadline = clk::now() + std::chrono::seconds( kDeadlineS );
        for ( int iter = 0; detaches < kRoundTrips && clk::now() < deadline; ++iter )
        {
            // Frame side: one step per Live channel; every third poll fails with EIO.
            for ( int s = 0; s < kChannels; ++s )
            {
                Channel& c = r.ch[s];
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
