#include <cerrno>
#include <sstream>
#include <string>

#include "check.hpp"
#include "core/geometry.hpp"
#include "core/log.hpp"
#include "source/source_channel.hpp"
#include "source/usb_node_locator.hpp"
#include "source_fakes.hpp"
#include "suites.hpp"

namespace
{
    using Channel = BasicSourceChannel<FakeSrc, FakeDec>;
    using State = Channel::State;
    using Attach = Channel::Attach;
    using Locator = BasicUsbNodeLocator<FakeHost>;

    // One channel over fakes, with its logs captured.
    struct Rig
    {
        explicit Rig( SourceKind kind = SourceKind::V4L2, int model = 640 ) : log( out, err ), loc( host, log ), ch( log, dev, dec )
        {
            host.present = { "/dev/video20" };
            host.usb = { 20 };
            ch.configure( 1, { kind, kind == SourceKind::V4L2 ? "/dev/video20" : "http://cam:8080/video" }, model, model );
        }
        Attach attach( bool hot )
        {
            std::string why;
            const Attach a = ch.attach( {}, loc, hot, why );
            last_why = why;
            return a;
        }
        // One full frame-path step: poll + decode; returns the decoded fd or -1.
        int frame()
        {
            EncodedFrame f {};
            if ( ch.poll( 0, f ) != FrameStatus::Frame )
                return -2;
            return ch.decode( f );
        }

    public:
        std::ostringstream out;
        std::ostringstream err;
        const Log log;
        FakeHost host;
        FakeDevice dev;
        FakeDecoderCtl dec;
        Locator loc;
        Channel ch;
        std::string last_why;
    };
}  // namespace

void test_source_channel( TestRun& t )
{
    // Startup attach: Live last, geometry and letterbox set, decoder initialized once, no warm-up.
    {
        Rig r;
        CHECK( t, r.ch.state() == State::NeverAttached );
        CHECK( t, r.attach( false ) == Attach::Live );
        CHECK( t, r.ch.live() && r.dev.opened == "/dev/video20" && r.dec.inits == 1 );
        const LbGeom lb = band_geom( 1280, 960, 640, 640 );
        CHECK( t, r.ch.geom().w == 1280 && r.ch.geom().h == 960 && r.ch.geom().aux_fd == -1 );
        CHECK( t, r.ch.geom().lb.top == lb.top && r.ch.geom().lb.h == lb.h && r.ch.geom().lb.w == lb.w );
        CHECK( t, r.out.str() == "source: geometry 1280x960\n" );
        CHECK( t, r.frame() == 7 );  // no warm-up at startup
    }
    // A failure at each step leaves the state as it was; the retry takes the same path.
    {
        Rig r;
        r.dev.open_rc = -1;
        CHECK( t, r.attach( false ) == Attach::Absent && r.ch.state() == State::NeverAttached );
        CHECK( t, r.last_why == "open refused" && r.dec.inits == 0 );

        r.dev.open_rc = 0;
        r.dec.init_rc = -1;
        CHECK( t, r.attach( true ) == Attach::DecoderFailed && r.ch.state() == State::NeverAttached );
        CHECK( t, r.dev.teardowns == 1 );  // the opened device is closed again
        CHECK( t, r.err.str().find( "probe s1: mpp decoder init failed - staying absent\n" ) != std::string::npos );

        r.dec.init_rc = 0;
        CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 2 );
        CHECK( t, r.out.str().find( "source s1: attached (hot) - geometry 1280x960\n" ) != std::string::npos );
    }
    // Locator miss: Absent with the reason; nothing opened.
    {
        Rig r;
        r.dev.trial_rc = -1;
        CHECK( t, r.attach( false ) == Attach::Absent && r.dev.opens == 0 );
        CHECK( t, r.last_why == "no usable USB capture node found" );
    }
    // Startup discovery: a missing configured node is replaced by the scanned one.
    {
        Rig r;
        r.host.present = { "/dev/video22" };
        r.host.usb = { 22 };
        CHECK( t, r.attach( false ) == Attach::Live && r.ch.node() == "/dev/video22" );
        CHECK( t, r.err.str().find( "camera: configured node /dev/video20 not present, falling back to USB discovery\n" ) != std::string::npos );
        CHECK( t, r.out.str().find( "camera: discovery adopted /dev/video22\n" ) != std::string::npos );
    }
    // Aux buffer: requested when the source is over 8x the model input; failure keeps the state.
    {
        Rig r( SourceKind::V4L2, 64 );
        r.dec.aux_rc = -1;
        CHECK( t, r.attach( true ) == Attach::AuxFailed && r.ch.state() == State::NeverAttached );
        CHECK( t, r.err.str().find( "probe s1: aux buffer alloc failed - staying absent\n" ) != std::string::npos );
        r.dec.aux_rc = 9;
        CHECK( t, r.attach( true ) == Attach::Live && r.ch.geom().aux_fd == 9 );
        CHECK( t, r.dec.inits == 1 );  // the decoder came up on the first try and is not re-initialized
    }
    // Detach -> re-attach keeps the decoder; a hot attach discards 3 valid decodes.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        r.ch.detach();
        CHECK( t, r.ch.state() == State::Detached );
        CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 1 );
        CHECK( t, r.out.str().find( "source s1: re-attached - geometry 1280x960\n" ) != std::string::npos );
        r.dec.decode_fd = -1;  // decode errors do not consume the warm-up
        CHECK( t, r.frame() == -1 && r.frame() == -1 );
        r.dec.decode_fd = 7;
        CHECK( t, r.frame() == -1 && r.frame() == -1 && r.frame() == -1 );
        CHECK( t, r.out.str().find( "camera: warm-up done, publishing resumed\n" ) != std::string::npos );
        CHECK( t, r.frame() == 7 );
        CHECK( t, r.dev.requeues == 6 );  // every dequeued buffer goes back
    }
    // Geometry guard: a re-attach with another size than the decoder's stays out.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        r.ch.detach();
        r.dev.w = 640;
        r.dev.h = 480;
        CHECK( t, r.attach( true ) == Attach::DecoderFailed && r.ch.state() == State::Detached );
        CHECK( t, r.err.str().find( "source s1: geometry changed 1280x960 -> 640x480 - decoder stays at 1280x960, not attaching\n" )
                          != std::string::npos );
        CHECK( t, r.dec.inits == 1 && r.ch.geom().w == 1280 );
    }
    // Empty packet: never decoded, requeued, logged; the warm-up is not consumed.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        r.dev.dq_size = 0;
        CHECK( t, r.frame() == -1 && r.dec.decodes == 0 && r.dev.requeues == 1 );
        CHECK( t, r.err.str().find( "camera: zero-size frame skipped\n" ) != std::string::npos );
    }
    // poll(): errno classes map to FrameStatus; the frame carries its source index.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        EncodedFrame f {};
        r.dev.dq_buf = 2;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Frame && f.src == 1 && f.buf == 2 && f.size == 100 );
        r.dev.dq_buf = -1;
        r.dev.dq_err = EAGAIN;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::NotReady );
        r.dev.dq_err = EINTR;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::NotReady );
        r.dev.dq_err = EIO;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Detached && f.err == EIO );
        r.dev.dq_err = ENODEV;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Detached );
        r.dev.dq_err = EINVAL;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Fatal && f.err == EINVAL );
    }
    // Net source: no node lookup (an empty /dev does not matter), the URL is opened as is.
    {
        Rig r( SourceKind::NetMjpeg );
        r.host.present.clear();
        r.host.usb.clear();
        CHECK( t, r.dev.net );
        CHECK( t, r.attach( false ) == Attach::Live && r.dev.opened == "http://cam:8080/video" && r.dev.trials.empty() );
    }
}
