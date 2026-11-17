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

}  // namespace

void test_global_track( TestRun& t )
{
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
}
