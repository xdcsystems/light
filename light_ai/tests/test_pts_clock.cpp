#include <cstdint>

#include "check.hpp"
#include "core/pts_clock.hpp"
#include "suites.hpp"

namespace
{
    void rescale( TestRun& t )
    {
        // Reference values from av_rescale( x, 90000, 1000000 ) (libavutil on
        // the board; the formula was checked against it for every x in
        // [-2e6, 2e6] and for 1e11-scale values).
        CHECK( t, us_to_90k( 0 ) == 0 );
        CHECK( t, us_to_90k( 5 ) == 0 );    // 0.45 -> 0
        CHECK( t, us_to_90k( 6 ) == 1 );    // 0.54 -> 1
        CHECK( t, us_to_90k( 50 ) == 5 );   // 4.5: halves away from zero
        CHECK( t, us_to_90k( -50 ) == -5 );
        CHECK( t, us_to_90k( -6 ) == -1 );
        CHECK( t, us_to_90k( 33333 ) == 3000 );
        CHECK( t, us_to_90k( 40000 ) == 3600 );
        CHECK( t, us_to_90k( 86400000000LL ) == 7776000000LL );  // one day
    }

    void cfr( TestRun& t )
    {
        PtsClock c( false );
        c.restart( 3600 );  // 25 fps in 1/90000
        CHECK( t, c.next( 123456, 0 ) == 0 );  // timestamps are ignored in CFR
        c.advance();
        CHECK( t, c.next( -1, 1 ) == 1 );
        c.advance();
        c.advance();  // a dropped frame still takes its slot
        CHECK( t, c.next( 0, 2 ) == 3 );
        CHECK( t, c.packet_ts( 3 ) == 10800 );
        // A new session restarts the counter.
        c.restart( 3000 );
        CHECK( t, c.next( 0, 99 ) == 0 );
        CHECK( t, c.packet_ts( 2 ) == 6000 );
        // A step below 1 is taken as 1; before any restart the step is 1.
        c.restart( 0 );
        CHECK( t, c.packet_ts( 7 ) == 7 );
        PtsClock fresh( false );
        CHECK( t, fresh.next( 0, 0 ) == 0 && fresh.packet_ts( 5 ) == 5 );
    }

    void session_filter( TestRun& t )
    {
        SessionPtsFilter f( true );
        // Session 1: frames submitted up to 10 minutes (54e6 ticks in 90kHz).
        f.on_epoch( 1 );
        int64_t fpts = 0;
        CHECK( t, f.next_frame_pts( 1000000, 0, fpts ) == 0 && fpts == 0 );
        int64_t out_ts = -1;
        CHECK( t, f.stamp_packet( true, 0, true, fpts, out_ts ) && out_ts == 0 );
        f.advance();

        const int64_t cpts_late = f.next_frame_pts( 601000000, 1, fpts );  // 600s -> 54e6 ticks
        CHECK( t, fpts == 54000000 && cpts_late == 54000000 );
        // Suppose cpts_late (and an async keyframe at 53996400) stay queued in MPP on disconnect.
        f.advance();

        // Session 2 (reconnect): epoch resets session PTS to 0 while codec PTS rebases above 54e6.
        f.on_epoch( 2 );  // muxer frame_step = 2
        int64_t fpts2 = -1;
        const int64_t cpts2_0 = f.next_frame_pts( 700000000, 2, fpts2 );
        CHECK( t, fpts2 == 0 && cpts2_0 == 54000001 );

        // Stale P-frame AND stale periodic IDR from Session 1 are both rejected cleanly!
        CHECK( t, !f.stamp_packet( true, 54000000, false, fpts2, out_ts ) );
        CHECK( t, !f.stamp_packet( true, 53996400, true, fpts2, out_ts ) );
        // Unlikely pre-IDR non-key inside new epoch is also rejected until the forced IDR lands.
        CHECK( t, !f.stamp_packet( true, 54000001, false, fpts2, out_ts ) );
        // New session's forced IDR lands with muxer PTS == 0 (never clamped to 54e6 + 1!).
        CHECK( t, f.stamp_packet( true, 54000001, true, fpts2, out_ts ) && out_ts == 0 );
        f.advance();

        // Next frame in Session 2 advances normally (3600 ticks * step 2 = 7200).
        const int64_t cpts2_1 = f.next_frame_pts( 700040000, 3, fpts2 );
        CHECK( t, fpts2 == 3600 && cpts2_1 == 54003601 );
        CHECK( t, f.stamp_packet( true, 54003601, false, fpts2, out_ts ) && out_ts == 7200 );
        // Duplicate/missing-PTS fallback keeps strict monotonicity.
        CHECK( t, f.stamp_packet( false, -1, false, fpts2, out_ts ) && out_ts == 7201 );

        // EAGAIN drain of an older missing-PTS packet using prev_fpts does not steal the new frame's PTS.
        SessionPtsFilter g( true );
        g.on_epoch( 1 );
        int64_t f0 = -1;
        CHECK( t, g.next_frame_pts( 1000000, 0, f0 ) == 0 && f0 == 0 );
        g.advance();
        int64_t f1 = -1;
        const int64_t c1 = g.next_frame_pts( 1040000, 1, f1 );
        CHECK( t, f1 == 3600 && c1 == 3600 );
        int64_t drained_ts = -1;
        CHECK( t, g.stamp_packet( false, -1, true, f0, drained_ts ) && drained_ts == 0 );
        int64_t new_ts = -1;
        CHECK( t, g.stamp_packet( true, c1, false, f1, new_ts ) && new_ts == 3600 );
    }

    void vfr( TestRun& t )
    {
        PtsClock c( true );
        c.restart( 1 );
        // The first timestamp is the session base.
        CHECK( t, c.next( 5000000, 0 ) == 0 );
        CHECK( t, c.next( 5040000, 1 ) == 3600 );
        CHECK( t, c.next( 5073333, 2 ) == 6600 );
        // Equal or earlier timestamps: strictly increasing anyway.
        CHECK( t, c.next( 5073333, 3 ) == 6601 );
        CHECK( t, c.next( 5000000, 4 ) == 6602 );
        // advance() does not touch the VFR pts.
        c.advance();
        CHECK( t, c.next( 5100000, 5 ) == 9000 );
        // No timestamp: the 25 fps grid over the frames sent so far (not per session).
        CHECK( t, c.next( -1, 10 ) == 36000 );
        // Monotonic guard continues across mixed ts_us >= 0 and ts_us < 0.
        CHECK( t, c.next( 5110000, 6 ) == 36001 );  // 9900 <= 36000 -> clamped to 36001
        CHECK( t, c.next( -1, 1 ) == 36002 );       // fallback 3600 <= 36001 -> clamped
        // A new session: a new base, and the monotonic clamp starts over.
        c.restart( 1 );
        CHECK( t, c.next( 9000000, 11 ) == 0 );
        CHECK( t, c.next( 9010000, 12 ) == 900 );
        CHECK( t, c.packet_ts( 900 ) == 900 );
    }
}  // namespace

void test_pts_clock( TestRun& t )
{
    rescale( t );
    cfr( t );
    vfr( t );
    session_filter( t );
}
