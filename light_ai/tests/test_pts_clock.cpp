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
    void queue_flush_continuity( TestRun& t )
    {
        // 1. Direct PtsClock rebase on queue flush: 600 ms stall collapses to 1 nominal step (50 ms = 4500 ticks).
        PtsClock c( true );
        c.restart( 1 );
        CHECK( t, c.next( 1000000, 0 ) == 0 );
        const int64_t p1 = c.next( 1050000, 1 );
        CHECK( t, p1 == 4500 );
        c.on_queue_flush();
        const int64_t p2 = c.next( 1650000, 2 );  // +600 ms wall-clock stall
        CHECK( t, p2 > p1 && ( p2 - p1 ) == 4500 );
        const int64_t p3 = c.next( 1700000, 3 );  // next normal 50 ms frame continues smoothly
        CHECK( t, p3 > p2 && ( p3 - p2 ) == 4500 );

        // 2. SessionPtsFilter across a 750 ms IDR cooldown (14 dropped P-frames at 50 ms before recovery IDR).
        SessionPtsFilter f( true );
        f.on_epoch( 1 );
        int64_t fpts = -1;
        int64_t out_ts = -1;
        const int64_t c0 = f.next_frame_pts( 1000000, 0, fpts );
        CHECK( t, f.stamp_packet( true, c0, true, fpts, out_ts ) && out_ts == 0 );
        f.advance();

        const int64_t c1 = f.next_frame_pts( 1050000, 1, fpts );
        int64_t prev_mux_ts = -1;
        CHECK( t, f.stamp_packet( true, c1, false, fpts, prev_mux_ts ) && prev_mux_ts == 4500 );
        f.advance();

        // Queue flush occurs after frame 1 (non-key): awaiting_key_ armed, PTS rebased across cooldown.
        f.on_queue_flush( false );
        int64_t prev_cpts = c1;
        uint64_t sent = 2;
        for ( int64_t ts_us = 1100000; ts_us <= 1750000; ts_us += 50000 )
        {
            const int64_t cpts = f.next_frame_pts( ts_us, sent++, fpts );
            CHECK( t, cpts > prev_cpts );
            prev_cpts = cpts;
            CHECK( t, !f.stamp_packet( true, cpts, false, fpts, out_ts ) );
            f.advance();
        }

        // Recovery IDR arrives at +750 ms (1,800,000 us): mux PTS step stays within 1-2 nominal steps (~50 ms).
        const int64_t c_idr = f.next_frame_pts( 1800000, sent++, fpts );
        CHECK( t, c_idr > prev_cpts );
        int64_t idr_mux_ts = -1;
        CHECK( t, f.stamp_packet( true, c_idr, true, fpts, idr_mux_ts ) );
        f.advance();
        const int64_t dt_idr = idr_mux_ts - prev_mux_ts;
        CHECK( t, idr_mux_ts > prev_mux_ts && dt_idr >= 4500 && dt_idr <= 9000 );

        // Subsequent P-frame (+50 ms at 1,850,000 us) is accepted and steps by exact 4500 ticks (50 ms).
        const int64_t c_next = f.next_frame_pts( 1850000, sent++, fpts );
        int64_t next_mux_ts = -1;
        CHECK( t, f.stamp_packet( true, c_next, false, fpts, next_mux_ts ) );
        CHECK( t, next_mux_ts > idr_mux_ts && ( next_mux_ts - idr_mux_ts ) == 4500 );

    }

    void queue_flush_edge_cases( TestRun& t )
    {
        SessionPtsFilter f( true );
        f.on_epoch( 1 );
        int64_t fpts = -1;
        int64_t out_ts = -1;
        uint64_t sent = 0;
        const int64_t c0 = f.next_frame_pts( 1000000, sent++, fpts, true );
        CHECK( t, f.stamp_packet( true, c0, true, fpts, out_ts ) );
        f.advance();
        const int64_t c1 = f.next_frame_pts( 1050000, sent++, fpts );
        int64_t next_mux_ts = -1;
        CHECK( t, f.stamp_packet( true, c1, false, fpts, next_mux_ts ) && next_mux_ts == 4500 );
        f.advance();

        // 3. EAGAIN / 1-frame codec lag: buffered P-frame drains AFTER recovery IDR is submitted.
        // flush_pending_ must NOT re-arm or collapse the post-IDR frame step to 1 tick.
        f.on_queue_flush( false );
        const int64_t c_lag_p = f.next_frame_pts( 1100000, sent++, fpts );
        CHECK( t, !f.stamp_packet( true, c_lag_p, false, fpts, out_ts ) );
        f.advance();
        // Recovery IDR submitted (+400 ms stall):
        int64_t fpts_idr_lag = -1;
        const int64_t c_idr_lag = f.next_frame_pts( 1500000, sent++, fpts_idr_lag, true );
        // Stale P-frame from MPP drains before IDR (EAGAIN path with prev_fpts):
        CHECK( t, !f.stamp_packet( true, c_lag_p, false, fpts, out_ts ) );
        // Then recovery IDR lands:
        int64_t lag_idr_ts = -1;
        CHECK( t, f.stamp_packet( true, c_idr_lag, true, fpts_idr_lag, lag_idr_ts ) );
        CHECK( t, lag_idr_ts > next_mux_ts && ( lag_idr_ts - next_mux_ts ) == 4501 );
        f.advance();
        // Next normal frame (+50 ms) MUST step by 4500 ticks, NOT collapse to +1 tick!
        const int64_t c_after_lag = f.next_frame_pts( 1550000, sent++, fpts );
        int64_t after_lag_ts = -1;
        CHECK( t, f.stamp_packet( true, c_after_lag, false, fpts, after_lag_ts ) );
        CHECK( t, ( after_lag_ts - lag_idr_ts ) == 4500 );

        // 4. Submit failure (!submitted) on recovery IDR preserves pending rebase for retried IDR.
        f.on_queue_flush( false );
        int64_t fpts_fail = -1;
        static_cast<void>( f.next_frame_pts( 1600000, sent++, fpts_fail, true ) );
        f.on_submit_failed( true );
        f.advance();
        int64_t fpts_retry = -1;
        const int64_t c_retry = f.next_frame_pts( 1850000, sent++, fpts_retry, true );  // +250 ms later
        int64_t retry_ts = -1;
        CHECK( t, f.stamp_packet( true, c_retry, true, fpts_retry, retry_ts ) );
        CHECK( t, retry_ts > after_lag_ts && ( retry_ts - after_lag_ts ) == 4501 );

        // 5. on_queue_flush(true) when flushing packet is already a keyframe + frame_step > 1.
        SessionPtsFilter h( true );
        h.on_epoch( 2 );  // muxer frame_step = 2
        int64_t hf = -1;
        int64_t h_ts0 = -1;
        const int64_t hc0 = h.next_frame_pts( 1000000, 0, hf, true );
        CHECK( t, h.stamp_packet( true, hc0, true, hf, h_ts0 ) && h_ts0 == 0 );
        h.advance();
        const int64_t hc1 = h.next_frame_pts( 1050000, 1, hf );
        int64_t h_ts1 = -1;
        CHECK( t, h.stamp_packet( true, hc1, true, hf, h_ts1 ) && h_ts1 == 9000 );
        h.on_queue_flush( true );  // keyframe already queued: awaiting_key_ stays false
        h.advance();
        const int64_t hc2 = h.next_frame_pts( 1650000, 2, hf );  // +600 ms stall
        int64_t h_ts2 = -1;
        CHECK( t, h.stamp_packet( true, hc2, false, hf, h_ts2 ) && h_ts2 == 18000 );  // +9000 (4500 * step 2)
        // Next normal frame expires flush_anchor_pts_, so a later !submitted does NOT clamp to +1 tick.
        h.advance();
        const int64_t hc3 = h.next_frame_pts( 1700000, 3, hf );
        int64_t h_ts3 = -1;
        CHECK( t, h.stamp_packet( true, hc3, false, hf, h_ts3 ) && h_ts3 == 27000 );
        h.on_submit_failed( false );
        const int64_t hc4 = h.next_frame_pts( 1750000, 4, hf );
        int64_t h_ts4 = -1;
        CHECK( t, h.stamp_packet( true, hc4, false, hf, h_ts4 ) && h_ts4 == 36000 );

        // 6. on_epoch() clears pending flush state; CFR mode rejects cooldown P-frames cleanly.
        h.on_queue_flush( false );
        h.on_epoch( 1 );
        const int64_t hc_ep = h.next_frame_pts( 2000000, 3, hf, true );
        int64_t h_ts_ep = -1;
        CHECK( t, h.stamp_packet( true, hc_ep, true, hf, h_ts_ep ) && h_ts_ep == 0 );

        SessionPtsFilter cfr_f( false );
        cfr_f.on_epoch( 3600 );
        int64_t cf = -1;
        int64_t cts = -1;
        const int64_t cc0 = cfr_f.next_frame_pts( 0, 0, cf, true );
        CHECK( t, cfr_f.stamp_packet( true, cc0, true, cf, cts ) && cts == 0 );
        cfr_f.advance();
        cfr_f.on_queue_flush( false );
        const int64_t cc1 = cfr_f.next_frame_pts( 0, 1, cf );
        CHECK( t, !cfr_f.stamp_packet( true, cc1, false, cf, cts ) );  // dropped during cooldown
        cfr_f.advance();
        const int64_t cc2 = cfr_f.next_frame_pts( 0, 2, cf, true );
        CHECK( t, cfr_f.stamp_packet( true, cc2, true, cf, cts ) && cts == 7200 );
    }
}  // namespace

void test_pts_clock( TestRun& t )
{
    rescale( t );
    cfr( t );
    vfr( t );
    session_filter( t );
    queue_flush_continuity( t );
    queue_flush_edge_cases( t );
}
