// The SourceChannel hand-over under two threads: a prober thread attaches
// non-Live channels, the frame thread polls, decodes, reads the geometry and
// detaches them again -- 1000+ round trips, last_ok stamped from both sides.
// Built twice: in light_ai_tests, and with -fsanitize=thread in
// light_ai_tests_tsan (any data race fails the gate).

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/log.hpp"
#include "source/source_channel.hpp"
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
        kRoundTrips = 1000,  // detach -> attach cycles, all channels together
        kDeadlineS = 30,
    };

    struct Bench
    {
        explicit Bench( const Log& log )
          : loc( host, log )
          , ch { { Channel( log, dev[0], dec[0] ), Channel( log, dev[1], dec[1] ), Channel( log, dev[2], dec[2] ) } }
        {
            host.present = { "/dev/video20", "/dev/video21", "/dev/video22" };
            host.usb = { 20, 21, 22 };
            for ( int s = 0; s < kChannels; ++s )
                ch[static_cast<size_t>( s )].configure( s, { SourceKind::V4L2, "/dev/video2" + std::to_string( s ) }, 640, 640 );
        }

        // Prober side: the busy list reads the other channels' node() -- attaching thread only.
        void probe_once()
        {
            for ( int s = 0; s < kChannels; ++s )
            {
                Channel& c = ch[static_cast<size_t>( s )];
                if ( c.live() )
                    continue;
                NodeList busy;
                for ( int k = 0; k < kChannels; ++k )
                {
                    if ( k != s )
                        busy.push_back( ch[static_cast<size_t>( k )].node() );
                }
                std::string why;
                if ( c.attach( busy, loc, true, why ) == Channel::Attach::Live )
                    attaches.fetch_add( 1 );
                c.stamp_ok( clk::now() );
            }
        }

        // Frame side: one step per Live channel; every third poll fails with EIO.
        void frame_once( int iter, long& geom_sum )
        {
            for ( int s = 0; s < kChannels; ++s )
            {
                Channel& c = ch[static_cast<size_t>( s )];
                if ( !c.live() )
                    continue;
                FakeDevice& d = dev[static_cast<size_t>( s )];
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
        }

    public:
        FakeHost host;
        std::array<FakeDevice, kChannels> dev {};
        std::array<FakeDecoderCtl, kChannels> dec {};
        Locator loc;
        std::array<Channel, kChannels> ch;
        std::atomic<int> attaches { 0 };
        int detaches = 0;  // frame thread only
    };
}  // namespace

void test_source_threads( TestRun& t )
{
    std::ostringstream out;
    std::ostringstream err;
    const Log log( out, err );
    Bench b( log );
    std::atomic<bool> quit { false };

    std::thread prober( [&] {
        while ( !quit.load() )
        {
            b.probe_once();
            std::this_thread::yield();
        }
    } );

    long geom_sum = 0;
    const auto deadline = clk::now() + std::chrono::seconds( kDeadlineS );
    for ( int iter = 0; b.detaches < kRoundTrips && clk::now() < deadline; ++iter )
    {
        b.frame_once( iter, geom_sum );
        std::this_thread::yield();
    }
    quit.store( true );
    prober.join();

    CHECK( t, b.detaches >= kRoundTrips );
    CHECK( t, b.attaches.load() >= b.detaches );  // every detach was followed by an attach (or is pending)
    CHECK( t, geom_sum > 0 );
    for ( const FakeDecoderCtl& d : b.dec )
        CHECK( t, d.inits == 1 );  // re-attaches never re-initialize the decoder
}
