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
        // Every live frame is encoded, however close.
        EncodePacer p( 1, 25 );
        CHECK( t, p.admit( 1000, 0, 1 ) );
        CHECK( t, p.admit( 1001, 0, 1 ) );
        CHECK( t, p.admit( 1001, 31, 1 ) );
    }

    void multi_cfr( TestRun& t )
    {
        // 25 fps: one encode per 40 ms; frames in between update the canvas only.
        EncodePacer p( 2, 25 );
        CHECK( t, p.admit( 100000, 0, 1 ) );  // the first frame of a session
        CHECK( t, !p.admit( 110000, 0, 1 ) );
        CHECK( t, !p.admit( 139999, 0, 1 ) );
        CHECK( t, p.admit( 140000, 0, 1 ) );  // measured from the last ENCODED frame
        CHECK( t, !p.admit( 170000, 9, 1 ) );  // queue depth does not matter in CFR
        // No timestamp: always encoded, and the next frame starts over.
        CHECK( t, p.admit( -1, 0, 1 ) );
        CHECK( t, p.admit( 170001, 0, 1 ) );
    }

    void multi_vfr( TestRun& t )
    {
        // Follow the source: 4 ms, 20 ms while the queue holds kBusyDepth or more.
        EncodePacer p( 3, 0 );
        CHECK( t, p.admit( 0, 0, 1 ) );
        CHECK( t, !p.admit( 3999, 0, 1 ) );
        CHECK( t, p.admit( 4000, 0, 1 ) );
        CHECK( t, !p.admit( 10000, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, p.admit( 10000, EncodePacer::kBusyDepth - 1, 1 ) );
        CHECK( t, !p.admit( 29999, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, p.admit( 30000, EncodePacer::kBusyDepth, 1 ) );
        CHECK( t, !p.admit( 69999, EncodePacer::kHeavyDepth, 1 ) );
        CHECK( t, p.admit( 70000, EncodePacer::kHeavyDepth, 1 ) );
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
}  // namespace

void test_encode_pacer( TestRun& t )
{
    single_view( t );
    multi_cfr( t );
    multi_vfr( t );
    epochs( t );
    idle( t );
    perf_buckets( t );
}
