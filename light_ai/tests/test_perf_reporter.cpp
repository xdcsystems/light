#include <array>
#include <chrono>
#include <cstdint>
#include <sstream>

#include "check.hpp"
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/perf_clock.hpp"
#include "core/perf_reporter.hpp"
#include "suites.hpp"

namespace
{
    // The two calls tick() makes per source: SourceSet::take_eagain_cnt and SourceChannel::skip_parts_cnt.
    struct FakeChan
    {
        uint64_t skip_parts_cnt() const noexcept { return skip; }

    public:
        uint64_t skip = 0;  // monotonic, as the channel's
    };

    struct FakeSrcs
    {
        int size() const { return n; }
        uint32_t take_eagain_cnt( int s )
        {
            const uint32_t v = eagain[static_cast<size_t>( s )];
            eagain[static_cast<size_t>( s )] = 0;
            return v;
        }
        FakeChan& operator[]( int s ) { return ch[static_cast<size_t>( s )]; }

    public:
        int n = 2;
        std::array<uint32_t, 4> eagain {};
        std::array<FakeChan, 4> ch {};
    };

    // OverlaySink's three perf calls; take_*() hand the window out and reset it, as the sink does.
    struct FakeSink
    {
        OverlayPerfWindow take_perf()
        {
            ++takes;
            const OverlayPerfWindow o = ovl;
            ovl.reset();
            return o;
        }
        EncPerfWindow take_enc_perf()
        {
            const EncPerfWindow e = enc;
            enc.reset();
            return e;
        }
        bool has_enc_perf() const { return has_enc; }

    public:
        OverlayPerfWindow ovl {};
        EncPerfWindow enc {};
        bool has_enc = true;
        int takes = 0;
    };

    PerfClock::time_point at_us( int64_t us )
    {
        return PerfClock::time_point( std::chrono::microseconds( 1000000 + us ) );
    }

    // The expected lines were printed by Pipeline::log_src_perf / log_enc_perf at b5dce21
    // from the same windows: the [perf] format is unchanged byte for byte (#110).
    void two_sources_two_windows( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        PerfReporter perf( true, log );
        FakeSrcs srcs;
        FakeSink sink;
        perf.start( at_us( 0 ) );

        // Window 1.
        perf.frame( 0, at_us( 10000 ) );
        perf.frame( 0, at_us( 43340 ) );  // dq gap 33.34 ms
        perf.decode( 0, 4.56 );
        perf.decode( 0, 2.0 );
        perf.frame( 1, at_us( 20000 ) );  // the first frame of s1: no gap
        perf.decode( 1, 7.04 );
        perf.orient( 1, 120.57 );
        perf.orient( 1, 80.0 );
        perf.frame( -1, at_us( 20000 ) );  // out of range: ignored
        perf.frame( 4, at_us( 20000 ) );
        perf.decode( 4, 99.0 );
        perf.step_done( at_us( 5000 ), at_us( 17460 ), srcs, sink );  // 12.46 ms
        perf.step_done( at_us( 20000 ), at_us( 21000 ), srcs, sink );
        perf.tick( at_us( 4999999 ), srcs, sink );  // the window is not over yet
        CHECK( t, out.str().empty() );
        CHECK_EQ( t, sink.takes, 0 );

        srcs.eagain = { 3, 0, 0, 0 };
        srcs.ch[0].skip = 5;
        sink.ovl.gpu_draw_ms_max = 1.23;
        sink.ovl.sync_cpu_ms_max = 0.47;
        sink.ovl.fence_tmo_cnt = 1;
        EncPerfWindow& e = sink.enc;
        e.enc_admit = 120;
        e.enc_skip = 3;
        e.idle_admit = 2;
        e.idle_skip = 1;
        for ( const double d : { 5.0, 15.0, 35.0, 75.0, 150.0, 300.0, 9.99 } )
            e.add_dt_enc( d );
        e.h264_ms_max = 6.78;
        e.compose_ms_max = 2.34;
        e.eagain_cnt = 4;
        e.pts_clamp_cnt = 1;
        e.q_flush_cnt = 2;
        perf.tick( at_us( 5000000 ), srcs, sink );

        // Window 2: every window and delta starts over, the dequeue gap does not.
        perf.frame( 0, at_us( 5010000 ) );  // 4966.66 ms after the last s0 frame
        srcs.ch[0].skip = 9;
        srcs.ch[1].skip = 2;
        srcs.eagain = { 0, 6, 0, 0 };
        perf.tick( at_us( 9000000 ), srcs, sink );  // 4 s into the window: nothing
        perf.tick( at_us( 10000000 ), srcs, sink );

        CHECK_STR_EQ( t, out.str(),
                      "[perf] src step_max=12.5 | s0 f=2 dq_max=33.3 eagain=3 dec_max=4.6 or_cnt=0 or_max=0.0 skip=5"
                      " | s1 f=1 dq_max=0.0 eagain=0 dec_max=7.0 or_cnt=2 or_max=120.6 skip=0\n"
                      "[perf] enc admit=120 skip=3 idle_a=2 idle_s=1 dt_max=300.0 dt_hist=[2,1,1,1,1,1] h264_max=6.8"
                      " cmp_max=2.3 eagain=4 clamp=1 qflush=2 | ovl gpu_max=1.2 sync_max=0.5 ftmo=1\n"
                      "[perf] src step_max=0.0 | s0 f=1 dq_max=4966.7 eagain=0 dec_max=0.0 or_cnt=0 or_max=0.0 skip=4"
                      " | s1 f=0 dq_max=0.0 eagain=6 dec_max=0.0 or_cnt=0 or_max=0.0 skip=2\n"
                      "[perf] enc admit=0 skip=0 idle_a=0 idle_s=0 dt_max=0.0 dt_hist=[0,0,0,0,0,0] h264_max=0.0"
                      " cmp_max=0.0 eagain=0 clamp=0 qflush=0 | ovl gpu_max=0.0 sync_max=0.0 ftmo=0\n" );
        CHECK_EQ( t, sink.takes, 2 );
        CHECK( t, err.str().empty() );

        // A detached source: its next frame has no gap to the old stream.
        out.str( "" );
        perf.source_reset( 0 );
        perf.source_reset( 7 );  // out of range: ignored
        perf.frame( 0, at_us( 10500000 ) );
        perf.frame( 0, at_us( 10520000 ) );
        perf.tick( at_us( 15000000 ), srcs, sink );
        CHECK( t, out.str().find( "[perf] src step_max=0.0 | s0 f=2 dq_max=20.0 eagain=0 " ) == 0 );
    }

    void one_source_no_encoder( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        PerfReporter perf( true, log );
        FakeSrcs srcs;
        srcs.n = 1;
        FakeSink sink;
        sink.has_enc = false;
        perf.start( at_us( 0 ) );
        perf.step_done( at_us( 0 ), at_us( 41060 ), srcs, sink );
        int64_t ts = 1000;
        perf.frame( 0, at_us( ts ) );
        for ( int i = 0; i < 148; ++i )
        {
            ts += 10000;
            perf.frame( 0, at_us( ts ) );
        }
        perf.frame( 0, at_us( ts + 66710 ) );  // 150 frames, the longest gap 66.71 ms
        perf.decode( 0, 3.33 );
        srcs.eagain = { 12, 0, 0, 0 };
        srcs.ch[0].skip = 1;
        sink.ovl.sync_cpu_ms_max = 2.71;
        perf.tick( at_us( 5000000 ), srcs, sink );
        CHECK_STR_EQ( t, out.str(),
                      "[perf] src step_max=41.1 | s0 f=150 dq_max=66.7 eagain=12 dec_max=3.3 or_cnt=0 or_max=0.0 skip=1\n"
                      "[perf] ovl gpu_max=0.0 sync_max=2.7 ftmo=0\n" );
    }

    // --net off: no hook collects, nothing is logged, the sink is never asked.
    void disabled( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        PerfReporter perf( false, log );
        FakeSrcs srcs;
        FakeSink sink;
        srcs.eagain = { 3, 0, 0, 0 };
        perf.start( at_us( 0 ) );
        perf.frame( 0, at_us( 1000 ) );
        perf.frame( 0, at_us( 2000 ) );
        perf.decode( 0, 1.0 );
        perf.orient( 0, 1.0 );
        perf.step_done( at_us( 0 ), at_us( 6000000 ), srcs, sink );
        perf.tick( at_us( 20000000 ), srcs, sink );
        CHECK( t, out.str().empty() && err.str().empty() );
        CHECK_EQ( t, sink.takes, 0 );
        CHECK_EQ( t, srcs.eagain[0], 3U );
    }

    void rt_telemetry( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        PerfReporter perf( true, log );
        FakeSrcs srcs;
        srcs.n = 1;
        FakeSink sink;
        sink.has_enc = false;
        perf.start( at_us( 0 ) );
        perf.enable_rt( 10.0 );
        perf.seed_rt_faults( 100, 1 );

        for ( int i = 0; i < 99; ++i )
            perf.step_done( at_us( i * 10000 ), at_us( i * 10000 + 5000 ), srcs, sink );
        perf.step_done( at_us( 990000 ), at_us( 990000 + 15000 ), srcs, sink );
        perf.note_rt_faults( 103, 1 );
        perf.tick( at_us( 5000000 ), srcs, sink );

        const std::string expected =
            "[perf] src step_max=15.0 | s0 f=0 dq_max=0.0 eagain=0 dec_max=0.0 or_cnt=0 or_max=0.0 skip=0\n"
            "[perf] ovl gpu_max=0.0 sync_max=0.0 ftmo=0\n"
            "[perf] rt step_p99=15.0 deadline_miss=1 minflt=3 majflt=0\n";
        CHECK_STR_EQ( t, out.str(), expected );
        CHECK( t, err.str().empty() );
    }
}  // namespace

void test_perf_reporter( TestRun& t )
{
    two_sources_two_windows( t );
    one_source_no_encoder( t );
    disabled( t );
    rt_telemetry( t );
}
