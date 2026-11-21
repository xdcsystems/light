#include <array>
#include <cerrno>
#include <sstream>
#include <string>

#include "check.hpp"
#include "core/auto_orient.hpp"
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
            Status why;
            const Attach a = ch.attach( {}, loc, hot, why );
            last_why = why.what();
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

    void aux_stride_sizing( TestRun& t )
    {
        // Stride forwarding and 16-aligned aux_fd stride sizing.
        {
            Rig r( SourceKind::V4L2, 64 );
            r.dev.w = 854;
            r.dev.h = 480;
            r.dec.hor_stride = 864;
            r.dec.ver_stride = 480;
            CHECK( t, r.attach( false ) == Attach::Live );
            CHECK( t, r.ch.hor_stride() == 864 && r.ch.ver_stride() == 480 );
            // 854 / 2 = 427 -> w_aux=428 -> 16-aligned stride 432; h_aux=240 -> 432 * 240 * 2 = 207360.
            CHECK( t, r.dec.last_aux_bytes == static_cast<size_t>( 432 ) * 240 * 2 );
        }
        {
            Rig r( SourceKind::V4L2, 64 );
            r.dev.w = 720;
            r.dev.h = 1280;
            r.dec.hor_stride = 720;
            r.dec.ver_stride = 1280;
            CHECK( t, r.attach( false ) == Attach::Live );
            // 720 / 2 = 360 -> 16-aligned stride 368; 1280 / 2 = 640 -> 368 * 640 * 2 = 471040.
            CHECK( t, r.dec.last_aux_bytes == static_cast<size_t>( 368 ) * 640 * 2 );
        }
        {
            Rig r( SourceKind::V4L2, 64 );
            r.dev.w = 1920;
            r.dev.h = 1080;
            r.dec.hor_stride = 1920;
            r.dec.ver_stride = 1088;
            CHECK( t, r.attach( false ) == Attach::Live );
            // 1920 / 2 = 960; 1080 / 2 = 540 -> 16-aligned stride 544 -> 960 * 544 * 2 = 1044480.
            CHECK( t, r.dec.last_aux_bytes == static_cast<size_t>( 960 ) * 544 * 2 );
        }
    }

    // A Live source's band is band_geom of its oriented size at its own
    // rotation: Pipeline::resize_into_slot computes the band as
    // band_geom( oriented_dims( raw, rot ) ) for every rot and relies on it
    // being SourceGeom::lb at rot == rot_deg (#110). Landscape, portrait, a
    // two-pass size (aux) and an odd one, at every rotation.
    void band_is_oriented_band_geom( TestRun& t )
    {
        constexpr std::array<std::array<int, 2>, 5> sizes { { { 640, 480 }, { 1920, 1080 }, { 1080, 1920 }, { 5120, 2880 },
                                                              { 1279, 719 } } };
        constexpr std::array<int, 4> rots { 0, 90, 180, 270 };
        for ( const std::array<int, 2>& sz : sizes )
        {
            for ( const int rot : rots )
            {
                Rig r;
                r.ch.configure( 1, { SourceKind::V4L2, "/dev/video20", rot, false }, 640, 640 );
                r.dev.w = sz[0];
                r.dev.h = sz[1];
                CHECK( t, r.attach( false ) == Attach::Live );
                const SourceGeom& g = r.ch.geom();
                CHECK( t, g.raw_w == sz[0] && g.raw_h == sz[1] && g.rot_deg == rot );
                int ow = 0;
                int oh = 0;
                auto_orient::oriented_dims( g.raw_w, g.raw_h, rot, ow, oh );
                CHECK( t, g.w == ow && g.h == oh );
                const LbGeom want = band_geom( ow, oh, 640, 640 );
                CHECK_EQ( t, g.lb.left, want.left );
                CHECK_EQ( t, g.lb.top, want.top );
                CHECK_EQ( t, g.lb.w, want.w );
                CHECK_EQ( t, g.lb.h, want.h );
            }
        }
    }

    // A re-attach at another size re-initializes the decoder and recomputes
    // the geometry, the band and the aux need (#117).
    void reattach_new_geometry( TestRun& t )
    {
        {
            Rig r;
            r.dev.w = 640;
            r.dev.h = 480;
            CHECK( t, r.attach( false ) == Attach::Live && r.dec.inits == 1 && r.ch.geom().aux_fd == -1 );
            const uint32_t seq = r.ch.attach_seq();
            r.ch.detach();
            r.dev.w = 1280;
            r.dev.h = 720;
            CHECK( t, r.attach( true ) == Attach::Live && r.ch.state() == State::Live && r.dec.inits == 2 );
            CHECK( t, r.out.str().find( "source s1: geometry changed 640x480 -> 1280x720, re-initializing the decoder\n" )
                          != std::string::npos );
            const SourceGeom& g = r.ch.geom();
            CHECK( t, g.raw_w == 1280 && g.raw_h == 720 && g.w == 1280 && g.h == 720 && g.aux_fd == -1 );
            const LbGeom lb = band_geom( 1280, 720, 640, 640 );
            CHECK( t, g.lb.left == lb.left && g.lb.top == lb.top && g.lb.w == lb.w && g.lb.h == lb.h );
            CHECK( t, r.ch.attach_seq() == seq + 1u );  // the frame thread resets orientation and tracks
            // The same size again: the decoder stays.
            r.ch.detach();
            CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 2 );
        }
        // The aux buffer of the old size is dropped and requested again for the new one.
        {
            Rig r( SourceKind::NetMjpeg, 64 );
            r.dev.w = 640;
            r.dev.h = 480;
            CHECK( t, r.attach( false ) == Attach::Live && r.ch.geom().aux_fd == 9 && r.dec.aux_calls == 1 );
            CHECK( t, r.dec.last_aux_bytes == rga_limits::aux_bytes( 640, 480 ) );
            r.ch.detach();
            r.dev.w = 1280;
            r.dev.h = 720;
            CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 2 && r.dec.aux_calls == 2 );
            CHECK( t, r.ch.geom().aux_fd == 9 && r.dec.last_aux_bytes == rga_limits::aux_bytes( 1280, 720 ) );
            // A failed re-init stays absent and tries again on the next attach.
            r.ch.detach();
            r.dev.w = 800;
            r.dev.h = 600;
            r.dec.init_rc = 1;
            CHECK( t, r.attach( true ) == Attach::DecoderFailed && r.ch.state() == State::Detached && r.dec.inits == 3 );
            r.dec.init_rc = 0;
            CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 4 && r.ch.geom().raw_w == 800 );
            CHECK( t, r.dec.last_aux_bytes == rga_limits::aux_bytes( 800, 600 ) );
        }
    }
}  // namespace

void test_source_channel( TestRun& t )
{
    // Startup attach: Live last, geometry and letterbox set, decoder initialized once;
    // V4L2 warms up on cold start (AE settling after STREAMON).
    {
        Rig r;
        CHECK( t, r.ch.state() == State::NeverAttached );
        CHECK( t, r.attach( false ) == Attach::Live );
        CHECK( t, r.ch.live() && r.dev.opened == "/dev/video20" && r.dec.inits == 1 );
        const LbGeom lb = band_geom( 1280, 960, 640, 640 );
        CHECK( t, r.ch.geom().w == 1280 && r.ch.geom().h == 960 && r.ch.geom().aux_fd == -1 );
        CHECK( t, r.ch.geom().lb.top == lb.top && r.ch.geom().lb.h == lb.h && r.ch.geom().lb.w == lb.w );
        CHECK( t, r.out.str() == "source: geometry 1280x960\n" );
        CHECK( t, r.frame() == -1 && r.frame() == -1 && r.frame() == -1 );  // V4L2 cold-start warm-up
        CHECK( t, r.out.str().find( "source s1: warm-up done, publishing resumed\n" ) != std::string::npos );
        CHECK( t, r.frame() == 7 );
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
        const std::string fallback = "camera: configured node /dev/video20 not present, falling back to USB discovery\n";
        CHECK( t, r.err.str().find( fallback ) != std::string::npos );
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
        CHECK( t, r.out.str().find( "source s1: warm-up done, publishing resumed\n" ) != std::string::npos );
        CHECK( t, r.frame() == 7 );
        CHECK( t, r.dev.requeues == 6 );  // every dequeued buffer goes back
    }
    // Empty packet: never decoded, requeued, logged; the warm-up is not consumed.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        r.dev.dq_size = 0;
        CHECK( t, r.frame() == -1 && r.dec.decodes == 0 && r.dev.requeues == 1 );
        CHECK( t, r.err.str().find( "source s1: zero-size frame skipped\n" ) != std::string::npos );
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
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Detached && f.st.sys() == EIO );
        r.dev.dq_err = ENODEV;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Detached );
        r.dev.dq_err = EINVAL;
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Fatal && f.st.sys() == EINVAL );
    }
    // Net source: no node lookup (an empty /dev does not matter), URL opened as is, no cold warm-up.
    {
        Rig r( SourceKind::NetMjpeg );
        r.host.present.clear();
        r.host.usb.clear();
        CHECK( t, r.dev.net );
        CHECK( t, r.attach( false ) == Attach::Live && r.dev.opened == "http://cam:8080/video" && r.dev.trials.empty() );
        CHECK( t, r.frame() == 7 );
    }
    // detach() tears down the source immediately; consecutive decoder errors surface Errc::Lost.
    {
        Rig r;
        CHECK( t, r.attach( false ) == Attach::Live );
        const int before = r.dev.teardowns;
        r.ch.detach();
        CHECK( t, r.dev.teardowns == before + 1 );

        CHECK( t, r.attach( true ) == Attach::Live );
        EncodedFrame f {};
        Status err;
        r.dec.decode_rc = 1;  // Timeout
        for ( int k = 0; k < 3; ++k )
        {
            CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Frame );
            CHECK( t, r.ch.decode( f, &err ) == -1 && err.ok() );
        }
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Frame );
        CHECK( t, r.ch.decode( f, &err ) == -1 && err.code() == Errc::Lost );

        // NoMemory triggers Lost immediately and resets dec_ready_ for clean re-init.
        r.dec.decode_rc = 2;
        err = {};
        CHECK( t, r.ch.poll( 0, f ) == FrameStatus::Frame );
        CHECK( t, r.ch.decode( f, &err ) == -1 && err.code() == Errc::Lost );
        r.ch.detach();
        r.dec.decode_rc = 0;
        CHECK( t, r.attach( true ) == Attach::Live && r.dec.inits == 2 );
    }
    aux_stride_sizing( t );
    band_is_oriented_band_geom( t );
    reattach_new_geometry( t );
}
