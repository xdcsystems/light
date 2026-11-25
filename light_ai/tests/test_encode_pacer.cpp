#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "check.hpp"
#include "core/encode_pacer.hpp"
#include "core/net_perf.hpp"
#include "suites.hpp"

namespace
{
    void single_view( TestRun& t )
    {
        // VFR (fps <= 0): every live frame is encoded, however close.
        EncodePacer vfr( 1, 0 );
        CHECK( t, vfr.admit( 1000, 0, 1 ) );
        CHECK( t, vfr.admit( 1001, 0, 1 ) );
        CHECK( t, vfr.admit( 1001, EncodePacer::kBusyDepth - 1, 1 ) );
        CHECK( t, !vfr.admit( 1002, EncodePacer::kPauseDepth, 1 ) );

        // CFR (fps > 0): single camera respects 1000000 / fps pacing.
        EncodePacer cfr( 1, 25 );
        CHECK( t, cfr.admit( 100000, 0, 1 ) );
        CHECK( t, !cfr.admit( 110000, 0, 1 ) );
        CHECK( t, !cfr.admit( 139999, 0, 1 ) );
        CHECK( t, cfr.admit( 140000, 0, 1 ) );
    }

    void multi_cfr( TestRun& t )
    {
        // 25 fps: one encode per 40 ms; frames in between update the canvas only.
        EncodePacer p( 2, 25 );
        CHECK( t, p.admit( 100000, 0, 1 ) );  // the first frame of a session
        CHECK( t, !p.admit( 110000, 0, 1 ) );
        CHECK( t, !p.admit( 139999, 0, 1 ) );
        CHECK( t, p.admit( 140000, 0, 1 ) );  // measured from the last ENCODED frame
        CHECK( t, !p.admit( 170000, 5, 1 ) );  // queue depth below pause does not speed up CFR
        // No timestamp: always encoded, and the next frame starts over.
        CHECK( t, p.admit( -1, 0, 1 ) );
        CHECK( t, p.admit( 170001, 0, 1 ) );
    }

    void multi_vfr( TestRun& t )
    {
        // Follow the source: 33.3 ms (~30 fps), 40 ms (25 fps) at kHeavyDepth or more.
        EncodePacer p( 3, 0 );
        CHECK( t, p.admit( 0, 0, 1 ) );
        CHECK( t, !p.admit( 33332, 0, 1 ) );
        CHECK( t, p.admit( 33333, 0, 1 ) );
        CHECK( t, !p.admit( 66665, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, p.admit( 66666, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, !p.admit( 106665, EncodePacer::kHeavyDepth, 1 ) );
        CHECK( t, p.admit( 106666, EncodePacer::kHeavyDepth, 1 ) );
    }

    void epochs( TestRun& t )
    {
        // A new session: its first live frame is always encoded, whatever the
        // stamp of the previous session.
        EncodePacer p( 2, 25 );
        CHECK( t, p.admit( 0, 0, 1 ) );
        CHECK( t, !p.admit( 1000, 0, 1 ) );
        CHECK( t, p.admit( 1000, 0, 2 ) );
        CHECK( t, !p.admit( 2000, 0, 2 ) );
        // The same holds when the previous stamp lies in the future (sources
        // with different clocks): the new epoch drops it.
        CHECK( t, p.admit( 900000, 0, 2 ) );
        CHECK( t, p.admit( 500000, 0, 3 ) );
    }

    void idle( TestRun& t )
    {
        EncodePacer p( 2, 25 );
        // Idle frames share the budget with live ones.
        CHECK( t, p.admit_idle( 100000, 0, 1 ) );
        CHECK( t, !p.admit( 110000, 0, 1 ) );
        CHECK( t, p.admit( 140000, 0, 1 ) );
        // A busy queue skips the idle tick and leaves the stamp alone.
        CHECK( t, !p.admit_idle( 150000, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, !p.admit( 170000, 0, 1 ) );
        CHECK( t, p.admit( 180000, 0, 1 ) );
        // Idle frames are encoded however close, and in single view too.
        CHECK( t, p.admit_idle( 180001, 0, 1 ) );
        EncodePacer s( 1, 25 );
        CHECK( t, s.admit_idle( 0, EncodePacer::kBusyDepth - 1, 1 ) );
        CHECK( t, !s.admit_idle( 1, EncodePacer::kBusyDepth, 1 ) );
        // An idle frame in a new session stamps that session: a live frame right after it waits.
        CHECK( t, p.admit_idle( 0, 0, 2 ) );
        CHECK( t, !p.admit( 1000, 0, 2 ) );
        // A skipped idle tick still enters the new session: the stale stamp is dropped.
        CHECK( t, !p.admit_idle( 5000, EncodePacer::kBusyDepth, 3 ) );
        CHECK( t, p.admit( 6000, 0, 3 ) );
        // [M-1]: out-of-order or negative admit_idle must not rewind a newer last_ts_us_.
        CHECK( t, p.admit_idle( 4000, 0, 3 ) );
        CHECK( t, p.admit_idle( -1, 0, 3 ) );
        CHECK( t, !p.admit( 45000, 0, 3 ) );
        CHECK( t, p.admit( 46000, 0, 3 ) );
    }

    // The encoder's perf histograms (net_perf.hpp): an edge value opens the next bucket of the < table and closes
    // its bucket of the <= table; NaN lands in the last bucket of both.
    void slowest_source_and_grid( TestRun& t )
    {
        // s0 at 25 fps (40 ms), s1 at ~30 fps (33.3 ms) with a 189 ms Wi-Fi spike:
        // median rejects the spike and paces to s0 (40 ms = 25 fps).
        EncodePacer p( 2, 0 );
        for ( int k = 0; k < 6; ++k )
            p.observe_source( 0, static_cast<int64_t>( k ) * 40000 );
        const int64_t s1_ts[6] = { 0, 33333, 66666, 255666, 288999, 322332 };
        for ( const int64_t ts : s1_ts )
            p.observe_source( 1, ts );
        CHECK_EQ( t, p.slowest_period_us( 322332 ), 40000 );
        CHECK( t, p.admit( 322332, 0, 1 ) );
        CHECK( t, !p.admit( 362331, 0, 1 ) );
        // Late tick (+2 ms): grid stays phase-locked at 362332, so next tick is 402332 (38 ms later).
        CHECK( t, p.admit( 364332, 0, 1 ) );
        CHECK( t, !p.admit( 402331, 0, 1 ) );
        CHECK( t, p.admit( 402332, 0, 1 ) );
        // Stall (>1 full period late): resyncs grid without a catch-up burst.
        CHECK( t, p.admit( 520000, 0, 1 ) );
        CHECK( t, !p.admit( 559999, 0, 1 ) );
        CHECK( t, p.admit( 560000, 0, 1 ) );
        // Night-mode s0 slows to 20 fps (50 ms); pacer follows to 50 ms.
        int64_t t0 = 600000;
        for ( int k = 0; k < 5; ++k )
        {
            t0 += 50000;
            p.observe_source( 0, t0 );
        }
        CHECK_EQ( t, p.slowest_period_us( t0 ), 50000 );
        // Silent s0 (>1 s stale) no longer holds back live s1 (~33.3 ms).
        int64_t t1 = t0 + 1100000;
        for ( int k = 0; k < 4; ++k )
        {
            t1 += 33333;
            p.observe_source( 1, t1 );
        }
        CHECK_EQ( t, p.slowest_period_us( t1 ), 33333 );
        // Reconnecting s0 after >1 s stale pause resets its old 50 ms history.
        t1 += 1500000;
        p.observe_source( 0, t1 );
        p.observe_source( 0, t1 + 33333 );
        p.observe_source( 0, t1 + 66666 );
        CHECK_EQ( t, p.slowest_period_us( t1 + 66666 ), 33333 );
        // Out-of-order timestamp behind grid is rejected without advancing grid.
        CHECK( t, p.admit( t1 + 100000, 0, 1 ) );
        CHECK( t, !p.admit( t1 + 90000, 0, 1 ) );
    }

    void perf_buckets( TestRun& t )
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        EncPerfWindow enc;  // <10, 10-30, 30-50, 50-100, 100-250, >=250 ms
        const std::array<double, 11> enc_ms { 9.99, 10.0, 29.9, 30.0, 50.0, 99.0, 100.0, 249.0, 250.0, 1e9, nan };
        for ( const double ms : enc_ms )
            enc.add_dt_enc( ms );
        const std::array<uint32_t, 6> enc_want { 1, 2, 1, 2, 2, 3 };
        for ( size_t i = 0; i < enc_want.size(); ++i )
            CHECK_EQ( t, enc.dt_enc_hist[i], enc_want[i] );

        MuxPerfWindow mux;  // <=5, 5-20, 20-100, >100 ms
        const std::array<double, 7> mux_ms { 5.0, 5.01, 20.0, 20.5, 100.0, 100.1, nan };
        for ( const double ms : mux_ms )
            mux.add_mux_write( ms );
        const std::array<uint32_t, 4> mux_want { 1, 2, 2, 2 };
        for ( size_t i = 0; i < mux_want.size(); ++i )
            CHECK_EQ( t, mux.mux_write_hist[i], mux_want[i] );
    }
    void backpressure_hysteresis( TestRun& t )
    {
        // Pause-on-critical and hysteresis resume (8 -> 6).
        EncodePacer vfr( 1, 0 );
        CHECK( t, vfr.admit( 100000, 0, 1 ) );
        CHECK( t, !vfr.admit( 200000, EncodePacer::kPauseDepth, 1 ) );
        CHECK( t, vfr.backpressure_paused() );
        CHECK_EQ( t, vfr.backpressure_level(), 3 );
        // Still paused above kHeavyDepth.
        CHECK( t, !vfr.admit( 400000, EncodePacer::kHeavyDepth + 1, 1 ) );
        CHECK( t, vfr.backpressure_paused() );
        // Resumes once drained to <= kHeavyDepth (6), still throttled at level 3 (150 ms).
        CHECK( t, vfr.admit( 500000, EncodePacer::kHeavyDepth, 1 ) );
        CHECK( t, !vfr.backpressure_paused() );
        CHECK( t, !vfr.admit( 600000, 0, 1 ) );  // < 150 ms while at level 3
        CHECK( t, vfr.admit( 650000, 0, 1 ) );   // >= 150 ms

        // Anti-oscillation: 2 s clean is not enough to recover (< 8 s hold-down).
        CHECK( t, vfr.admit( 2650000, EncodePacer::kLowDepth, 1 ) );
        CHECK_EQ( t, vfr.backpressure_level(), 3 );
        // Continuous 8 s clean steps down one level at a time (3 -> 2 -> 1 -> 0).
        CHECK( t, vfr.admit( 8700000, EncodePacer::kLowDepth, 1 ) );
        CHECK_EQ( t, vfr.backpressure_level(), 2 );
        CHECK( t, vfr.admit( 16800000, EncodePacer::kLowDepth, 1 ) );
        CHECK_EQ( t, vfr.backpressure_level(), 1 );
        CHECK( t, vfr.admit( 24900000, EncodePacer::kLowDepth, 1 ) );
        CHECK_EQ( t, vfr.backpressure_level(), 0 );

        // Gradual step-down with 200 ms hold / 500 ms cooldown and immediate flush escalation.
        EncodePacer mp( 2, 0 );
        CHECK( t, mp.admit( 0, 0, 1 ) );
        CHECK( t, mp.admit( 100000, EncodePacer::kBusyDepth, 1 ) );    // transient < 200 ms
        CHECK_EQ( t, mp.backpressure_level(), 0 );
        CHECK( t, mp.admit( 300000, EncodePacer::kBusyDepth, 1 ) );    // sustained >= 200 ms -> level 1
        CHECK_EQ( t, mp.backpressure_level(), 1 );
        CHECK( t, mp.admit( 600000, EncodePacer::kHeavyDepth, 1 ) );   // cooldown < 500 ms
        CHECK_EQ( t, mp.backpressure_level(), 1 );
        CHECK( t, mp.admit( 800000, EncodePacer::kHeavyDepth, 1 ) );   // sustained >= 200 ms + cooldown >= 500 ms -> level 2
        CHECK_EQ( t, mp.backpressure_level(), 2 );
        mp.observe_flush( 850000, 1 );
        CHECK_EQ( t, mp.backpressure_level(), 3 );

        // Age-driven backpressure works even at low depth (e.g. throttled FPS before 300 ms qflush).
        EncodePacer age_p( 2, 0 );
        CHECK( t, age_p.admit( 0, 0, 1, 0 ) );
        CHECK( t, age_p.admit( 100000, 2, 1, EncodePacer::kBusyAgeUs ) );  // seeds hold timer
        CHECK_EQ( t, age_p.backpressure_level(), 0 );
        CHECK( t, age_p.admit( 300000, 2, 1, EncodePacer::kBusyAgeUs ) );  // >= 200 ms -> level 1
        CHECK_EQ( t, age_p.backpressure_level(), 1 );
        CHECK( t, !age_p.admit_idle( 350000, 1, 1, EncodePacer::kBusyAgeUs ) );  // idle skipped on busy age
        CHECK( t, !age_p.admit( 400000, 2, 1, EncodePacer::kPauseAgeUs ) );      // pauses before 300 ms qflush
        CHECK( t, age_p.backpressure_paused() );
        CHECK_EQ( t, age_p.backpressure_level(), 3 );
        // Stays paused while age > kHeavyAgeUs even if depth <= kHeavyDepth.
        CHECK( t, !age_p.admit( 600000, 2, 1, EncodePacer::kHeavyAgeUs + 1 ) );
        CHECK( t, age_p.backpressure_paused() );
        // Resumes once age <= kHeavyAgeUs and depth <= kHeavyDepth.
        CHECK( t, age_p.admit( 700000, 2, 1, EncodePacer::kHeavyAgeUs ) );
        CHECK( t, !age_p.backpressure_paused() );
        // High age blocks recovery even when depth <= kLowDepth.
        CHECK( t, age_p.admit( 9000000, 1, 1, EncodePacer::kLowAgeUs + 1 ) );
        CHECK_EQ( t, age_p.backpressure_level(), 3 );
        CHECK( t, age_p.admit( 9200000, 1, 1, EncodePacer::kLowAgeUs ) );
        CHECK( t, age_p.admit( 17200000, 1, 1, EncodePacer::kLowAgeUs ) );
        CHECK_EQ( t, age_p.backpressure_level(), 2 );
        // Out-of-order stale timestamp must not seed or advance hysteresis timers.
        CHECK( t, mp.admit( 10000000, 0, 2 ) );
        CHECK( t, !mp.admit( 1000000, EncodePacer::kBusyDepth, 2 ) );  // stale (< 10 s)
        CHECK( t, mp.admit( 10050000, EncodePacer::kBusyDepth, 2 ) );
        CHECK_EQ( t, mp.backpressure_level(), 0 );  // only 50 ms since 10 s, not 9 s

        // ts_us < 0 at critical depth pauses and sets level 3.
        CHECK( t, !mp.admit( -1, EncodePacer::kPauseDepth, 2 ) );
        CHECK( t, mp.backpressure_paused() );
        CHECK_EQ( t, mp.backpressure_level(), 3 );

        // New session epoch resets backpressure cleanly.
        CHECK( t, mp.admit( 1300000, 0, 3 ) );
        CHECK_EQ( t, mp.backpressure_level(), 0 );
        CHECK( t, !mp.backpressure_paused() );
    }
}  // namespace

void test_encode_pacer( TestRun& t )
{
    single_view( t );
    multi_cfr( t );
    multi_vfr( t );
    epochs( t );
    idle( t );
    slowest_source_and_grid( t );
    perf_buckets( t );
    backpressure_hysteresis( t );
}
