// SourceSet over SourceChannels + fakes: configure, the round-robin from the
// cursor, non-Live channels skipped, errno -> FrameStatus, the 2 s stall
// detector, reset_stall_clocks, the blocking single-source dequeue, the
// absent mask and the busy-node list.

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "source/source_channel.hpp"
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
        kChannels = 3
    };
    using Set = BasicSourceSet<Channel, kChannels>;

    // Channel i runs on dev[i] / dec[i]; /dev/video20..22 present; logs captured.
    struct Rig
    {
        Rig() : log( out, err ), loc( host, log ), set( log, [this]( int i ) { return make( i ); } )
        {
            host.present = { "/dev/video20", "/dev/video21", "/dev/video22" };
            host.usb = { 20, 21, 22 };
        }
        Channel make( int i ) { return Channel( log, dev[static_cast<size_t>( i )], dec[static_cast<size_t>( i )] ); }
        void configure_v4l2( int n )
        {
            std::vector<SourceEndpoint> eps;
            for ( int s = 0; s < n; ++s )
                eps.push_back( { SourceKind::V4L2, "/dev/video2" + std::to_string( s ) } );
            set.configure( eps, 640, 640 );
        }
        // n channels configured and Live (startup attach); every dequeue yields a frame.
        void live( int n )
        {
            configure_v4l2( n );
            for ( int s = 0; s < n; ++s )
            {
                std::string why;
                set[s].attach( {}, loc, false, why );
                dev[static_cast<size_t>( s )].dq_buf = 0;
            }
        }
        void fail_all( int err_no )
        {
            for ( FakeDevice& d : dev )
            {
                d.dq_buf = -1;
                d.dq_err = err_no;
            }
        }

    public:
        std::ostringstream out;
        std::ostringstream err;
        const Log log;
        FakeHost host;
        std::array<FakeDevice, kChannels> dev {};
        std::array<FakeDecoderCtl, kChannels> dec {};
        Locator loc;
        Set set;
    };

    void configure( TestRun& t )
    {
        Rig r;
        bool threw = false;
        try
        {
            r.configure_v4l2( kChannels + 1 );
        }
        catch ( const std::runtime_error& e )
        {
            threw = std::string( e.what() ) == "pipeline: at most 3 sources are supported (one per NPU core)";
        }
        CHECK( t, threw );
        CHECK( t, r.set.size() == 0 );

        Rig z;
        z.configure_v4l2( 0 );
        CHECK( t, z.set.size() == 0 && !z.set.any_live() );
        CHECK( t, z.out.str().find( "sources configured: none - waiting for attach" ) != std::string::npos );

        Rig q;
        q.configure_v4l2( 2 );
        CHECK( t, q.set.size() == 2 && !q.set.any_live() );
        std::string why;
        CHECK( t, q.set[1].attach( {}, q.loc, false, why ) == Channel::Attach::Live );
        CHECK( t, q.dev[1].opens == 1 && q.dev[0].opens == 0 );  // channel i is built on dev[i]
        CHECK( t, q.set[1].index() == 1 && q.set[1].node() == "/dev/video21" );
        CHECK( t, q.set.any_live() );
    }

    void round_robin( TestRun& t )
    {
        Rig r;
        r.live( kChannels );
        int seq[6] = {};
        for ( int& s : seq )
        {
            EncodedFrame f {};
            CHECK( t, r.set.next( 0, f ) == FrameStatus::Frame );
            s = f.src;
            static_cast<void>( r.set[f.src].decode( f ) );
        }
        CHECK( t, seq[0] == 1 && seq[1] == 2 && seq[2] == 0 && seq[3] == 1 && seq[4] == 2 && seq[5] == 0 );  // from cursor + 1

        r.set[1].detach();  // not Live: skipped
        bool saw1 = false;
        for ( int k = 0; k < 6; ++k )
        {
            EncodedFrame f {};
            CHECK( t, r.set.next( 0, f ) == FrameStatus::Frame );
            saw1 = saw1 || f.src == 1;
            static_cast<void>( r.set[f.src].decode( f ) );
        }
        CHECK( t, !saw1 );
        CHECK( t, r.dev[1].dequeues == 2 );  // only the first round polled it
    }

    void errno_mapping( TestRun& t )
    {
        Rig r;
        r.live( kChannels );
        r.fail_all( EAGAIN );
        EncodedFrame f {};
        CHECK( t, r.set.next( 0, f ) == FrameStatus::NotReady );
        CHECK( t, r.dev[0].dequeues == 1 && r.dev[1].dequeues == 1 && r.dev[2].dequeues == 1 );  // one full round

        r.dev[2].dq_err = ENODEV;
        f = {};
        CHECK( t, r.set.next( 0, f ) == FrameStatus::Detached && f.src == 2 && f.err == ENODEV );

        r.dev[1].dq_err = EBADF;
        f = {};
        CHECK( t, r.set.next( 0, f ) == FrameStatus::Fatal && f.src == 1 && f.err == EBADF );  // the cursor was 2: s0, s1
    }

    void stall_detector( TestRun& t )
    {
        Rig r;
        r.live( kChannels );
        r.fail_all( EAGAIN );
        r.set[1].stamp_ok( clk::now() - std::chrono::seconds( 3 ) );
        EncodedFrame f {};
        CHECK( t, r.set.next( 0, f ) == FrameStatus::Detached && f.src == 1 && f.err == EIO );
        CHECK( t, r.err.str().find( "source s1: no frame in 2000 ms - treating as recoverable stall" ) != std::string::npos );

        r.set[1].stamp_ok( clk::now() - std::chrono::seconds( 3 ) );
        r.set.reset_stall_clocks( clk::now() );
        f = {};
        CHECK( t, r.set.next( 0, f ) == FrameStatus::NotReady );  // the clocks restarted: no stall
    }

    void blocking( TestRun& t )
    {
        Rig r;
        r.live( 1 );
        EncodedFrame f {};
        CHECK( t, r.set.next_blocking( f ) == FrameStatus::Frame && f.src == 0 );
        static_cast<void>( r.set[0].decode( f ) );
        r.dev[0].dq_buf = -1;
        r.dev[0].dq_err = EIO;
        f = {};
        CHECK( t, r.set.next_blocking( f ) == FrameStatus::Detached && f.err == EIO );
        r.set[0].detach();
        const int dq = r.dev[0].dequeues;
        f = {};
        CHECK( t, r.set.next_blocking( f ) == FrameStatus::NotReady && r.dev[0].dequeues == dq );  // not Live: no dequeue

        Rig z;
        z.configure_v4l2( 0 );
        f = {};
        CHECK( t, z.set.next_blocking( f ) == FrameStatus::NotReady );
    }

    void absent_mask( TestRun& t )
    {
        Rig r;
        r.live( 2 );
        std::array<bool, kChannels> m {};
        CHECK( t, !r.set.absent_mask( m ) && !m[0] && !m[1] && !m[2] );  // all configured Live; s2 not configured
        r.set[1].detach();
        CHECK( t, r.set.absent_mask( m ) && !m[0] && m[1] && !m[2] );

        Rig z;
        z.configure_v4l2( 0 );
        m = { { true, true, true } };
        CHECK( t, !z.set.absent_mask( m ) && !m[0] && !m[1] && !m[2] );
    }

    void busy_nodes( TestRun& t )
    {
        Rig r;
        r.set.configure( { { SourceKind::V4L2, "/dev/video20" },
                           { SourceKind::V4L2, "/dev/video21" },
                           { SourceKind::NetMjpeg, "http://cam:8080/video" } },
                         640, 640 );
        const NodeList b0 = r.set.busy_nodes( 0 );
        CHECK( t, b0.size() == 1 && b0[0] == "/dev/video21" );  // self and the net source left out
        const NodeList b2 = r.set.busy_nodes( 2 );
        CHECK( t, b2.size() == 2 && node_listed( b2, "/dev/video20" ) && node_listed( b2, "/dev/video21" ) );

        Rig q;
        q.configure_v4l2( 1 );
        CHECK( t, q.set.busy_nodes( 0 ).empty() );  // only the configured channels count
    }
}  // namespace

void test_source_set( TestRun& t )
{
    configure( t );
    round_robin( t );
    errno_mapping( t );
    stall_detector( t );
    blocking( t );
    absent_mask( t );
    busy_nodes( t );
}
