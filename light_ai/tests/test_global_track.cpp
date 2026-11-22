#include <cmath>

#include "check.hpp"
#include "core/global_track.hpp"
#include "suites.hpp"

namespace
{
    ReidResult make_res( int src, int lid, uint32_t gen, int64_t ts, float a, float b, float c = 0.0f ) noexcept
    {
        ReidResult r {};
        r.src_idx = src;
        r.local_id = lid;
        r.gen = gen;
        r.ts_us = ts;
        r.dim = 4;
        r.ok = true;
        r.feat[0] = a;
        r.feat[1] = b;
        r.feat[2] = c;
        r.feat[3] = 0.0f;
        l2_normalize( r.feat.data(), 4 );
        return r;
    }

    void check_ch9_reid_gating( TestRun& t )
    {
        // Clipped / occluded tracks never reach the Re-ID queue: select_crops() refuses them up
        // front. Refused tracks come first, the clean one last, so the crop cap cannot hide a
        // refusal; the clean one is the positive control (same size, unbound, retry interval due).
        GlobalTrackTable tab( 0.62f );
        {
            TrajList gate {};
            // Touches the left band edge.
            gate.push( TrackState { 11, TRACK_CONF, 30.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 11, 180.0f } );
            // Squeezed below its height prior (120 < 0.85 * 180).
            gate.push( TrackState { 12, TRACK_CONF, 500.0f, 360.0f, 0.0f, 0.0f, 80.0f, 120.0f, 3, 0, 12, 180.0f } );
            // An overlapping pair (IoU 0.6 >= 0.30): both refused.
            gate.push( TrackState { 13, TRACK_CONF, 800.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 13, 180.0f } );
            gate.push( TrackState { 14, TRACK_CONF, 820.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 14, 180.0f } );
            // Clean.
            gate.push( TrackState { 15, TRACK_CONF, 300.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 15, 180.0f } );
            tab.sync_cam( 2, gate, 100000 );
            ReidCropReq reqs[2] {};
            CHECK( t, tab.select_crops( 2, gate, { 0, 640, 80, 480 }, 1280, 720, 128, 256, reqs, 2 ) == 1 );
            CHECK( t, reqs[0].local_id == 15 && reqs[0].gen == 15 );
            TrajList none {};
            tab.sync_cam( 2, none, 110000 );
            CHECK( t, tab.active_global_count() == 0 );
        }

        // Populate gallery with 3 clean tracks: G1=[1,0,0], G2=[0,1,0], G3=[0,0,1].
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 200.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        cam0.push( TrackState { 2, TRACK_CONF, 600.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 0, cam0, 120000 );
        tab.apply_result( make_res( 0, 1, 1, 130000, 1.0f, 0.0f, 0.0f ) );
        tab.apply_result( make_res( 0, 2, 2, 130000, 0.0f, 1.0f, 0.0f ) );
        TrajList empty0 {};
        tab.sync_cam( 0, empty0, 140000 );
        TrajList cam1 {};
        cam1.push( TrackState { 3, TRACK_CONF, 400.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 3, 180.0f } );
        tab.sync_cam( 1, cam1, 150000 );
        tab.apply_result( make_res( 1, 3, 3, 150000, 0.0f, 0.0f, 1.0f ) );
        TrajList empty1 {};
        tab.sync_cam( 1, empty1, 160000 );

        // Bind cam1 track 4 to G1.
        TrajList cam1b {};
        cam1b.push( TrackState { 4, TRACK_CONF, 400.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 4, 180.0f } );
        tab.sync_cam( 1, cam1b, 200000 );
        tab.apply_result( make_res( 1, 4, 4, 200000, 1.0f, 0.0f, 0.0f ) );
        tab.sync_cam( 1, cam1b, 210000 );
        CHECK( t, cam1b.d[0].global_id == 1 );

        // Ambiguous bound switch (G2 vs G3 within kReidMargin=0.06) is blocked even after 2 hits.
        tab.apply_result( make_res( 1, 4, 4, 250000, 0.0f, 0.72f, 0.69f ) );
        tab.apply_result( make_res( 1, 4, 4, 260000, 0.0f, 0.72f, 0.69f ) );
        tab.sync_cam( 1, cam1b, 270000 );
        CHECK( t, cam1b.d[0].global_id == 1 );
    }

    // Low §3: the 64-slot gallery saturates without overflow -- the 65th
    // identity evicts the oldest inactive one (count stays 64); the evicted
    // appearance comes back as a new id, a kept one re-identifies.
    void check_saturation( TestRun& t )
    {
        constexpr int dim = 128;
        const auto one_hot = []( int lid, int64_t ts, int axis ) {
            ReidResult r {};
            r.src_idx = 0;
            r.local_id = lid;
            r.gen = static_cast<uint32_t>( lid );
            r.ts_us = ts;
            r.dim = dim;
            r.ok = true;
            r.feat[static_cast<size_t>( axis )] = 1.0f;
            return r;
        };
        const auto person = []( int lid ) {
            return TrackState { lid, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, static_cast<uint32_t>( lid ), 180.0f };
        };
        GlobalTrackTable tab( 0.62f );
        const TrajList empty {};
        // One identity at a time: bind, then leave (inactive, kept for the 45 s TTL).
        const auto show = [&]( int lid, int axis, int64_t ts ) {
            TrajList cam {};
            cam.push( person( lid ) );
            tab.sync_cam( 0, cam, ts );
            tab.apply_result( one_hot( lid, ts, axis ) );
            tab.sync_cam( 0, cam, ts + 1000 );
            const int gid = cam.d[0].global_id;
            TrajList gone = empty;
            tab.sync_cam( 0, gone, ts + 2000 );
            return gid;
        };
        int64_t ts = 1000000;
        bool ids_ok = true;
        for ( int i = 0; i < reid_cfg::kMaxGlobalTracks; ++i, ts += 100000 )
            ids_ok = ids_ok && show( i + 1, i, ts ) == i + 1;
        CHECK( t, ids_ok );
        CHECK( t, tab.active_global_count() == reid_cfg::kMaxGlobalTracks );

        CHECK( t, show( 100, 80, ts ) == reid_cfg::kMaxGlobalTracks + 1 );  // the 65th evicts the oldest (id 1)
        CHECK( t, tab.active_global_count() == reid_cfg::kMaxGlobalTracks );
        ts += 100000;
        CHECK( t, show( 101, 5, ts ) == 6 );                                  // axis 5 is still in the gallery
        ts += 100000;
        CHECK( t, show( 102, 0, ts ) == reid_cfg::kMaxGlobalTracks + 2 );   // axis 0 was evicted: a new id
        CHECK( t, tab.active_global_count() == reid_cfg::kMaxGlobalTracks );
    }

    void check_m2_disconnect_cleanup( TestRun& t )
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 0, cam0, 100000 );
        tab.apply_result( make_res( 0, 1, 1, 100000, 1.0f, 0.0f ) );
        CHECK( t, tab.active_global_count() == 1 );

        TrajList empty {};
        tab.sync_cam( 0, empty, 200000 );
        tab.sync_cam( 0, empty, 200000 + 46LL * 1000 * 1000 );
        CHECK( t, tab.active_global_count() == 0 );
    }

    // 5. last_seen_us does not move backwards when a second camera has an older timestamp.
    void check_last_seen_monotonic( TestRun& t )
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 0, cam0, 1000000 );
        tab.apply_result( make_res( 0, 1, 1, 1000000, 1.0f, 0.0f ) );
        tab.sync_cam( 0, cam0, 50000000 );

        TrajList cam1 {};
        cam1.push( TrackState { 2, TRACK_CONF, 400.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 1, cam1, 10000000 );
        tab.apply_result( make_res( 1, 2, 2, 10000000, 1.0f, 0.0f ) );
        tab.sync_cam( 1, cam1, 10000000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        const TrajList empty {};
        TrajList e1 = empty;
        tab.sync_cam( 1, e1, 11000000 );
        TrajList e0 = empty;
        tab.sync_cam( 0, e0, 50500000 );
        // 56 s is > 45 s past cam1's 10 s, but only 6 s past cam0's 50 s -> must NOT evict.
        e0 = empty;
        tab.sync_cam( 0, e0, 56000000 );
        CHECK( t, tab.active_global_count() == 1 );
        // 96 s is > 45 s past 50 s -> now evicted.
        e0 = empty;
        tab.sync_cam( 0, e0, 96000000 );
        CHECK( t, tab.active_global_count() == 0 );
    }

    // 6. select_crops() picks highest (need_init, frames_since_reid) even at tail of trajs.
    void check_select_crops_priority( TestRun& t )
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 200.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        cam0.push( TrackState { 2, TRACK_CONF, 450.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        cam0.push( TrackState { 3, TRACK_CONF, 700.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 3, 180.0f } );
        cam0.push( TrackState { 4, TRACK_CONF, 950.0f, 360.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 4, 180.0f } );

        int64_t ts = 100000;
        tab.sync_cam( 0, cam0, ts );
        tab.apply_result( make_res( 0, 1, 1, ts, 1.0f, 0.0f, 0.0f ) );
        tab.apply_result( make_res( 0, 2, 2, ts, 0.0f, 1.0f, 0.0f ) );
        tab.apply_result( make_res( 0, 3, 3, ts, 0.0f, 0.0f, 1.0f ) );
        tab.refresh_global_ids( 0, cam0 );

        tab.mark_dispatched( 0, 1, 1 );
        for ( int f = 0; f < 16; ++f )
        {
            ts += 33000;
            tab.sync_cam( 0, cam0, ts );
        }
        tab.mark_dispatched( 0, 2, 2 );
        for ( int f = 0; f < 15; ++f )
        {
            ts += 33000;
            tab.sync_cam( 0, cam0, ts );
        }

        // Tail track 4 is unbound (need_init=true); tail track 3 has highest age (46 > 31 > 15).
        ReidCropReq reqs[2] {};
        CHECK( t, tab.select_crops( 0, cam0, { 0, 640, 80, 480 }, 1280, 720, 128, 256, reqs, 2 ) == 2 );
        CHECK( t, reqs[0].local_id == 4 && reqs[1].local_id == 3 );

        // Once track 4 binds and resets, highest frames_since_reid (3=46, 1=31) wins over head track 2 (15).
        ReidResult r4 {};
        r4.src_idx = 0;
        r4.local_id = 4;
        r4.gen = 4;
        r4.ts_us = ts;
        r4.dim = 4;
        r4.ok = true;
        r4.feat[3] = 1.0f;
        tab.apply_result( r4 );
        tab.mark_dispatched( 0, 4, 4 );
        CHECK( t, tab.select_crops( 0, cam0, { 0, 640, 80, 480 }, 1280, 720, 128, 256, reqs, 2 ) == 2 );
        CHECK( t, reqs[0].local_id == 3 && reqs[1].local_id == 1 );
    }

    // 7. sync_cam before apply_result + refresh_global_ids: dead track result creates no phantom.
    void check_refresh_global_ids_dead_track( TestRun& t )
    {
        GlobalTrackTable tab( 0.62f );
        TrajList f0 {};
        f0.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 0, f0, 100000 );

        TrajList f1 {};
        f1.push( TrackState { 2, TRACK_CONF, 600.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 0, f1, 200000 );
        tab.apply_result( make_res( 0, 1, 1, 150000, 1.0f, 0.0f ) );
        CHECK( t, tab.active_global_count() == 0 );

        tab.apply_result( make_res( 0, 2, 2, 200000, 0.0f, 1.0f ) );
        tab.refresh_global_ids( 0, f1 );
        CHECK( t, tab.active_global_count() == 1 );
        CHECK( t, f1.d[0].global_id == 1 );
    }
}  // namespace

void test_global_track( TestRun& t )
{
    check_saturation( t );
    // 1. Person seen on cam 0, then matched across to cam 1 -> shared global_id.
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 0, cam0, 100000 );
        tab.apply_result( make_res( 0, 1, 1, 100000, 1.0f, 0.0f ) );
        tab.sync_cam( 0, cam0, 140000 );
        CHECK( t, cam0.d[0].global_id == 1 );

        TrajList cam1 {};
        cam1.push( TrackState { 5, TRACK_CONF, 400.0f, 320.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 10, 180.0f } );
        tab.sync_cam( 1, cam1, 150000 );
        tab.apply_result( make_res( 1, 5, 10, 150000, 0.96f, 0.28f ) );
        tab.sync_cam( 1, cam1, 190000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        // Short drop & respawn on cam 0: old local binding clears cleanly on disappearance
        // so the respawned local track re-attaches to global_id == 1.
        TrajList empty0 {};
        tab.sync_cam( 0, empty0, 200000 );
        TrajList respawn0 {};
        respawn0.push( TrackState { 2, TRACK_CONF, 310.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 0, respawn0, 240000 );
        tab.apply_result( make_res( 0, 2, 2, 240000, 0.98f, 0.20f ) );
        tab.sync_cam( 0, respawn0, 280000 );
        CHECK( t, respawn0.d[0].global_id == 1 );
    }

    // 2. Collision guard on the same camera: two simultaneous live tracks on cam 0
    // with similar appearance cannot take the same global_id.
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 150.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        cam0.push( TrackState { 2, TRACK_CONF, 500.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 0, cam0, 100000 );
        tab.apply_result( make_res( 0, 1, 1, 100000, 1.0f, 0.0f ) );
        tab.apply_result( make_res( 0, 2, 2, 100000, 1.0f, 0.0f ) );
        tab.sync_cam( 0, cam0, 140000 );
        CHECK( t, cam0.d[0].global_id == 1 && cam0.d[1].global_id == 2 );
    }

    // 3. Switch hysteresis: requires 2 consecutive higher-margin matches to switch global_id.
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 200.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        cam0.push( TrackState { 2, TRACK_CONF, 700.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 2, 180.0f } );
        tab.sync_cam( 0, cam0, 100000 );
        tab.apply_result( make_res( 0, 1, 1, 100000, 1.0f, 0.0f ) );
        tab.apply_result( make_res( 0, 2, 2, 100000, 0.0f, 1.0f ) );
        TrajList empty0 {};
        tab.sync_cam( 0, empty0, 120000 );  // deactivate cam0 bindings while keeping gallery alive

        TrajList cam1 {};
        cam1.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 1, cam1, 200000 );
        tab.apply_result( make_res( 1, 1, 1, 200000, 0.90f, 0.44f ) );  // binds to global 1
        tab.sync_cam( 1, cam1, 210000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        // 1st contradictory observation matching global 2 does NOT switch immediately.
        tab.apply_result( make_res( 1, 1, 1, 250000, 0.0f, 1.0f ) );
        tab.sync_cam( 1, cam1, 260000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        // Contradictory observation BELOW sim_thr does NOT switch even if > cur_sim + 0.10.
        tab.apply_result( make_res( 1, 1, 1, 240000, 0.30f, 0.40f, 0.86f ) );
        tab.apply_result( make_res( 1, 1, 1, 245000, 0.30f, 0.40f, 0.86f ) );
        tab.sync_cam( 1, cam1, 246000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        // 1st strong observation matching global 2 (>= sim_thr) does NOT switch immediately.
        tab.apply_result( make_res( 1, 1, 1, 250000, 0.0f, 1.0f ) );
        tab.sync_cam( 1, cam1, 260000 );
        CHECK( t, cam1.d[0].global_id == 1 );

        // 2nd consecutive observation matching global 2 triggers the switch.
        tab.apply_result( make_res( 1, 1, 1, 300000, 0.0f, 1.0f ) );
        tab.sync_cam( 1, cam1, 310000 );
        CHECK( t, cam1.d[0].global_id == 2 );
    }

    // 4. Stale gallery eviction after 45 s + tiny crop (< 16x32) filtering.
    {
        GlobalTrackTable tab( 0.62f );
        TrajList cam0 {};
        cam0.push( TrackState { 1, TRACK_CONF, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 180.0f, 3, 0, 1, 180.0f } );
        tab.sync_cam( 0, cam0, 1000000 );
        tab.apply_result( make_res( 0, 1, 1, 1000000, 1.0f, 0.0f ) );
        CHECK( t, tab.active_global_count() == 1 );

        TrajList empty {};
        tab.sync_cam( 0, empty, 1000000 );
        tab.sync_cam( 0, empty, 1000000 + 46LL * 1000 * 1000 );
        CHECK( t, tab.active_global_count() == 0 );

        // Tiny crop (< 16x32 in tensor coords) is skipped cleanly.
        TrajList tiny {};
        tiny.push( TrackState { 7, TRACK_CONF, 100.0f, 100.0f, 0.0f, 0.0f, 10.0f, 20.0f, 3, 0, 1, 20.0f } );
        tab.sync_cam( 0, tiny, 2000000 );
        ReidCropReq reqs[2] {};
        CHECK( t, tab.select_crops( 0, tiny, { 0, 640, 80, 480 }, 1280, 720, 128, 256, reqs, 2 ) == 0 );
    }
    check_ch9_reid_gating( t );
    check_m2_disconnect_cleanup( t );

    check_last_seen_monotonic( t );
    check_select_crops_priority( t );
    check_refresh_global_ids_dead_track( t );
}
