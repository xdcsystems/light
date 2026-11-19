#include <array>
#include <cmath>
#include <cstdint>

#include "check.hpp"
#include "suites.hpp"
#include "tracker.hpp"

namespace
{
    enum : int64_t { kFrameUs = 40000 };  // 25 fps

    // Person detection from its center and size (Detection is top-left + size).
    Detection person( float cx, float cy, float w, float h )
    {
        return Detection { cx - w * 0.5f, cy - h * 0.5f, w, h, 0, 0.9f };
    }

    DetList one( const Detection& d )
    {
        DetList l;
        l.push( d );
        return l;
    }

    // Feeds the same standing person for `frames` frames starting at `ts`;
    // returns the timestamp of the next frame.
    int64_t stand( Tracker& trk, TrajList& out, int frames, int64_t ts )
    {
        for ( int i = 0; i < frames; ++i, ts += kFrameUs )
            trk.update( one( person( 500, 400, 100, 200 ) ), ts, out );
        return ts;
    }

    // A trail that ages out while the track lives restarts (#37): a confirmed
    // person stands still past kTrailMaxAgeMs (the step gate pushes nothing, the
    // spawn point ages out: n_pts 0 while CONF), then walks -- the trail grows again.
    void check_trail_restart( TestRun& t )
    {
        CHECK( t, Tracker {}.trail_max_age_us() == 4000000 );  // the 4 s the frame counts below rely on
        Tracker trk;
        TrajList out;
        int64_t ts = stand( trk, out, 101, 0 );  // ts 0 .. 4.0 s: the spawn point is still in
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF && out.d[0].n_pts == 1 );
        ts = stand( trk, out, 1, ts );  // 4.04 s: the spawn point ages out
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF && out.d[0].n_pts == 0 );
        for ( int i = 1; i <= 20; ++i, ts += kFrameUs )
            trk.update( one( person( 500.0f + 10.0f * static_cast<float>( i ), 400, 100, 200 ) ), ts, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
        CHECK( t, out.d[0].n_pts >= 2 );
    }

    // A tentative track expires straight to FREE (never enters TRACK_DEAD or hogs a slot).
    void check_walk_and_recent( TestRun& t )
    {
        Tracker trk;
        TrajList out;
        int64_t ts = 0;
        for ( int i = 0; i < 100; ++i, ts += kFrameUs )
            trk.update( one( person( 200.0f + 4.0f * static_cast<float>( i ), 400, 100, 200 ) ), ts, out );
        const float cx = 200.0f + 4.0f * 99.0f;  // the last fed position
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].cx, cx, 6.0f );
        CHECK_NEAR( t, out.d[0].cy, 400.0f, 1.0f );
        CHECK_NEAR( t, out.d[0].vx, 100.0f, 10.0f );
        CHECK_NEAR( t, out.d[0].vy, 0.0f, 5.0f );
        CHECK( t, out.d[0].n_pts > 10 );

        std::array<TrajPoint, 20> pts {};
        const int n = trk.recent_points( 1, 1, pts.data(), 20 );
        CHECK( t, n == 20 );  // capped by dst_cap
        bool ordered = true;
        for ( int k = 1; k < n; ++k )
            ordered = ordered && pts[k].ts_us > pts[k - 1].ts_us && pts[k].x > pts[k - 1].x;
        CHECK( t, ordered );
        CHECK_NEAR( t, pts[n - 1].x, cx, 20.0f );  // the newest point is always included

        std::array<TrajPoint, 20> sparse {};
        const int ns = trk.recent_points( 1, 4, sparse.data(), 20 );
        CHECK( t, ns == ( out.d[0].n_pts + 3 ) / 4 || ns == 20 );
        CHECK( t, sparse[ns - 1].ts_us == pts[n - 1].ts_us );  // stride keeps the newest

        CHECK( t, trk.recent_points( 99, 1, pts.data(), 20 ) == 0 );  // unknown id
        CHECK( t, trk.recent_points( 1, 0, pts.data(), 20 ) == 0 );   // bad stride
        CHECK( t, trk.recent_points( 1, 1, nullptr, 20 ) == 0 );
        CHECK( t, trk.recent_points( 1, 1, pts.data(), 0 ) == 0 );
    }

    void check_ch10_dead_eviction( TestRun& t )
    {
        Tracker trk;
        TrajList out;
        int64_t ts = 0;
        for ( int f = 0; f < 3; ++f, ts += kFrameUs )
        {
            DetList crowd;
            for ( int i = 0; i < 8; ++i )
                crowd.push( person( 100.0f + static_cast<float>( i ) * 140.0f, 300.0f, 60.0f, 120.0f ) );
            trk.update( crowd, ts, out );
        }
        // Drop track idx 2 (id==3) first so it becomes oldest TRACK_DEAD (ts_last_match_us = 2*kFrameUs).
        for ( int f = 0; f < 7; ++f, ts += kFrameUs )
        {
            DetList seven;
            for ( int i = 0; i < 8; ++i )
            {
                if ( i != 2 )
                    seven.push( person( 100.0f + static_cast<float>( i ) * 140.0f, 300.0f, 60.0f, 120.0f ) );
            }
            trk.update( seven, ts, out );
        }
        for ( int f = 0; f < 7; ++f, ts += kFrameUs )
            trk.update( DetList {}, ts, out );
        CHECK( t, out.n == 8 );

        // 9th spawn must evict oldest TRACK_DEAD (id==3), leaving 7 DEAD + 1 TENT (id==9).
        trk.update( one( person( 1250.0f, 600.0f, 60.0f, 120.0f ) ), ts, out );
        bool saw_new = false;
        bool saw_id3 = false;
        for ( int k = 0; k < out.n; ++k )
        {
            saw_new = saw_new || ( out.d[k].id == 9 && out.d[k].state == TRACK_TENT );
            saw_id3 = saw_id3 || ( out.d[k].id == 3 );
        }
        CHECK( t, out.n == 8 && saw_new && !saw_id3 );

        const uint32_t prev_gen = out.d[0].gen;
        trk.reset();
        trk.update( one( person( 500.0f, 400.0f, 80.0f, 160.0f ) ), ts + kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].gen > prev_gen );
    }

    void check_ch11_clipped_ramp( TestRun& t )
    {
        // Sweep cy across [394..406] with bh=100 (delta in [+12..-12]); smooth ramp keeps steps small.
        float prev_cy = 0.0f;
        float max_step = 0.0f;
        for ( int step = 0; step <= 12; ++step )
        {
            Tracker trk;
            TrajList out;
            const int64_t ts = stand( trk, out, 5, 0 );
            const float det_cy = 394.0f + static_cast<float>( step );
            trk.update( one( person( 500.0f, det_cy, 100.0f, 100.0f ) ), ts, out );
            CHECK( t, out.n == 1 );
            if ( step > 0 )
                max_step = std::fmax( max_step, std::fabs( out.d[0].cy - prev_cy ) );
            prev_cy = out.d[0].cy;
        }
        CHECK( t, max_step < 15.0f );
    }

    void check_tent_expiry( TestRun& t )
    {
        Tracker trk;
        TrajList out;
        trk.update( one( person( 500, 400, 100, 200 ) ), 0, out );
        trk.update( DetList {}, 1000000, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_TENT );
        trk.update( DetList {}, 1040000, out );
        CHECK( t, out.n == 0 );

        // 8 transient 1-frame false positives must not starve a 9th person after 1.04 s.
        DetList crowd;
        for ( int i = 0; i < 8; ++i )
            crowd.push( person( 100.0f + static_cast<float>( i ) * 120.0f, 300.0f, 60.0f, 120.0f ) );
        trk.update( crowd, 2000000, out );
        CHECK( t, out.n == 8 );
        trk.update( DetList {}, 3040000, out );
        CHECK( t, out.n == 0 );
        trk.update( one( person( 600, 400, 100, 200 ) ), 3080000, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_TENT );
    }

    // P2-9: Round-2 clipped box bottom-anchoring without size mutation + gentle downward hp decay.
    void check_p2_9( TestRun& t )
    {
        {
            Tracker trk;
            TrajList out;
            int64_t ts = stand( trk, out, 5, 0 );  // cx=500, cy=400, w=100, h=200 (feet at y=500)
            // Squeezed upper-clipped box: h=80 -> rh=0.40 (< kSizeMin 0.50 -> round 2 only),
            // feet stay at y=500 -> cy_det = 460. Expect cy near 400 and bh/bw frozen.
            trk.update( one( person( 500, 460, 100, 80 ) ), ts, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
            CHECK_NEAR( t, out.d[0].bh, 200.0f, 1e-2f );
            CHECK_NEAR( t, out.d[0].bw, 100.0f, 1e-2f );
            CHECK_NEAR( t, out.d[0].cy, 400.0f, 5.0f );
        }
        // Bottom-clipped box (legs occluded by a desk): top stays at y=300, h=80 -> cy_det=340.
        // Expect top-anchored cy near 400 instead of jumping up toward 280.
        {
            Tracker trk;
            TrajList out;
            int64_t ts = stand( trk, out, 5, 0 );  // cx=500, cy=400, w=100, h=200 (top at y=300)
            trk.update( one( person( 500, 340, 100, 80 ) ), ts, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
            CHECK_NEAR( t, out.d[0].bh, 200.0f, 1e-2f );
            CHECK_NEAR( t, out.d[0].cy, 400.0f, 5.0f );
        }
        {
            Tracker trk;
            TrajList out;
            int64_t ts = 0;
            for ( int i = 0; i < 3; ++i, ts += kFrameUs )
                trk.update( one( person( 500, 300, 120, 400 ) ), ts, out );
            for ( int i = 0; i < 5; ++i, ts += kFrameUs )
                trk.update( one( person( 500, 400, 120, 200 ) ), ts, out );
            CHECK( t, out.n == 1 && out.d[0].bh > 350.0f );
            for ( int i = 0; i < 25; ++i, ts += kFrameUs )
                trk.update( one( person( 500, 400, 120, 200 ) ), ts, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].bh < 250.0f );
        }
    }

    void check_spawn_gates( TestRun& t )
    {
        // Nested sub-box suppression (kSpawnContainGate = 65%): a small head/torso
        // box (50x65) inside a full seated person (170x340) has IoU ~ 0.056 (< 0.15),
        // but IoA_min = 1.0, so it must not spawn a ghost person track either on the
        // same frame or against a live track.
        {
            Tracker trk;
            TrajList out;
            DetList dets;
            dets.push( person( 392.0f, 720.0f, 170.0f, 340.0f ) );
            dets.push( person( 368.0f, 605.0f, 50.0f, 65.0f ) );
            trk.update( dets, 0, out );
            CHECK( t, out.n == 1 );
            trk.update( one( person( 368.0f, 605.0f, 50.0f, 65.0f ) ), kFrameUs, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 );
        }
        // Spawn confidence gate (kInitConfPct = 35): a sub-0.35 detection cannot
        // spawn a new track, but can still update an existing track.
        {
            Tracker trk;
            TrajList out;
            Detection weak = person( 500, 400, 100, 200 );
            weak.conf = 0.28f;
            trk.update( one( weak ), 0, out );
            CHECK( t, out.n == 0 );
            Detection ok = person( 500, 400, 100, 200 );
            ok.conf = 0.36f;
            trk.update( one( ok ), kFrameUs, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 );
            trk.update( one( weak ), 2 * kFrameUs, out );
            CHECK( t, out.n == 1 && out.d[0].id == 1 );
        }
    }

    // M-14: a non-finite or empty person det neither spawns nor updates a
    // track; the live track keeps a finite state.
    void check_unusable_dets( TestRun& t )
    {
        Tracker trk;
        TrajList out;
        const float nan = std::nanf( "" );
        Detection bad_nan = person( 500, 400, 100, 200 );
        bad_nan.x = nan;
        Detection bad_w = person( 500, 400, 100, 200 );
        bad_w.w = 0.0f;
        Detection bad_inf = person( 500, 400, 100, 200 );
        bad_inf.h = INFINITY;
        trk.update( one( bad_nan ), 0, out );
        trk.update( one( bad_w ), kFrameUs, out );
        trk.update( one( bad_inf ), 2 * kFrameUs, out );
        CHECK( t, out.n == 0 );
        int64_t ts = stand( trk, out, 3, 3 * kFrameUs );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF );
        trk.update( one( bad_nan ), ts, out );
        CHECK( t, out.n == 1 && std::isfinite( out.d[0].cx ) && std::isfinite( out.d[0].cy ) && std::isfinite( out.d[0].bw ) );
        CHECK_NEAR( t, out.d[0].cx, 500.0f, 1.0f );
    }
}  // namespace

void test_tracker( TestRun& t )
{
    check_unusable_dets( t );
    // Lifecycle: tentative on the first sighting, confirmed on the 3rd match.
    {
        Tracker trk;
        TrajList out;
        trk.update( one( person( 500, 400, 100, 200 ) ), 0, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_TENT );
        CHECK_NEAR( t, out.d[0].cx, 500.0f, 1e-3f );
        CHECK_NEAR( t, out.d[0].cy, 400.0f, 1e-3f );
        CHECK( t, out.d[0].n_pts == 1 );  // spawn seeds the trail
        trk.update( one( person( 500, 400, 100, 200 ) ), kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_TENT );
        trk.update( one( person( 500, 400, 100, 200 ) ), 2 * kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].bw, 100.0f, 1e-2f );
        CHECK_NEAR( t, out.d[0].bh, 200.0f, 1e-2f );
    }
    // Preallocated buffer contract: out is cleared at entry.
    {
        Tracker trk;
        TrajList out;
        out.push( TrackState {} );
        out.push( TrackState {} );
        trk.update( DetList {}, 0, out );
        CHECK( t, out.n == 0 );
    }
    // Only persons (class 0) are tracked.
    {
        Tracker trk;
        TrajList out;
        Detection chair = person( 500, 400, 100, 200 );
        chair.cls = 56;
        trk.update( one( chair ), 0, out );
        CHECK( t, out.n == 0 );
    }
    // A confirmed track coasts through 6 missed frames (kMaxMiss = 6) and dies on the 7th;
    // the ghost keeps its trail until the points age out (4 s), then the slot is freed.
    {
        Tracker trk;
        TrajList out;
        int64_t ts = stand( trk, out, 5, 0 );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF );
        for ( int miss = 1; miss <= 6; ++miss, ts += kFrameUs )
        {
            trk.update( DetList {}, ts, out );
            CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF );
        }
        trk.update( DetList {}, ts, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_DEAD && out.d[0].id == 1 );
        trk.update( DetList {}, ts + 5000000, out );
        CHECK( t, out.n == 0 );
    }
    // A brief dropout does not change the identity.
    {
        Tracker trk;
        TrajList out;
        int64_t ts = stand( trk, out, 5, 0 );
        trk.update( DetList {}, ts, out );
        trk.update( DetList {}, ts + kFrameUs, out );
        stand( trk, out, 1, ts + 2 * kFrameUs );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
    }
    check_tent_expiry( t );
    check_walk_and_recent( t );
    // Two persons far apart keep two distinct stable ids.
    {
        Tracker trk;
        TrajList out;
        for ( int i = 0; i < 10; ++i )
        {
            DetList dets;
            dets.push( person( 300, 400, 100, 200 ) );
            dets.push( person( 900, 400, 100, 200 ) );
            trk.update( dets, i * kFrameUs, out );
        }
        CHECK( t, out.n == 2 );
        CHECK( t, out.d[0].id == 1 && out.d[1].id == 2 );
        CHECK( t, out.d[0].state == TRACK_CONF && out.d[1].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].cx, 300.0f, 1.0f );
        CHECK_NEAR( t, out.d[1].cx, 900.0f, 1.0f );
    }
    // Spawn dedupe: an unmatched detection overlapping a live track spawns nothing.
    {
        Tracker trk;
        TrajList out;
        DetList dets;
        dets.push( person( 500, 400, 100, 200 ) );
        dets.push( person( 505, 400, 100, 200 ) );
        trk.update( dets, 0, out );
        CHECK( t, out.n == 1 );
    }
    // Size-consistency gate: a box squeezed below 40% of the track size does not
    // match; it overlaps the track, so it is deduped instead of spawning.
    {
        Tracker trk;
        TrajList out;
        const int64_t ts = stand( trk, out, 5, 0 );
        trk.update( one( person( 500, 400, 30, 200 ) ), ts, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 );
        CHECK_NEAR( t, out.d[0].bw, 100.0f, 1e-2f );  // the squeezed box never reached the filter
    }
    check_spawn_gates( t );
    // Capacity: 8 track slots; the 9th person is dropped.
    {
        Tracker trk;
        TrajList out;
        DetList dets;
        for ( int i = 0; i < 9; ++i )
            dets.push( person( 100.0f + 300.0f * static_cast<float>( i ), 400, 100, 200 ) );
        trk.update( dets, 0, out );
        CHECK( t, out.n == 8 );
        CHECK( t, out.d[7].id == 8 );
    }
    // reset() restarts the id sequence.
    {
        Tracker trk;
        TrajList out;
        stand( trk, out, 3, 0 );
        trk.reset();
        trk.update( DetList {}, 0, out );
        CHECK( t, out.n == 0 );
        trk.update( one( person( 500, 400, 100, 200 ) ), kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 );
    }
    check_p2_9( t );
    check_trail_restart( t );
    check_ch10_dead_eviction( t );
    check_ch11_clipped_ramp( t );
}
