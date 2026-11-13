#include <cmath>
#include <cstdint>

#include "box_mapper.h"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    // 1280x960 scene in a 640x640 tensor: band rows [80, 560), scale 0.5.
    constexpr LbGeom kBand { 0, 640, 80, 480 };
    constexpr float kScale = 0.5f;

    DetList one( float x, float y, float w, float h, int cls )
    {
        DetList l;
        l.push( Detection { x, y, w, h, cls, 0.9f } );
        return l;
    }

    bool color( const GpuBox& b, uint8_t r, uint8_t g, uint8_t bl )
    {
        return b.r == r && b.g == g && b.b == bl;
    }
}  // namespace

void test_box_mapper( TestRun& t )
{
    const Tracker idle;  // no tracks: recent_points() serves nothing
    const TrajList no_tracks;
    GpuBoxList boxes;

    // A non-person detection maps scene -> tensor: scale, then the band offset.
    fill_gpu_boxes( one( 100, 200, 300, 400, 56 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 );
    CHECK_NEAR( t, boxes.d[0].x0, 50.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y0, 180.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].x1, 200.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y1, 380.0f, 1e-4f );
    CHECK( t, color( boxes.d[0], 64, 255, 64 ) && boxes.d[0].a == 255 );  // chair

    fill_gpu_boxes( one( 100, 200, 300, 400, 2 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 && color( boxes.d[0], 255, 255, 0 ) );  // any other class

    // Preallocated buffer contract: boxes is cleared at entry.
    fill_gpu_boxes( DetList {}, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // Persons are drawn from the track list only, never from the detection list.
    fill_gpu_boxes( one( 100, 200, 300, 400, 0 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // A box fully outside the band is rejected; a partial one is clamped to the band.
    fill_gpu_boxes( one( 2000, 200, 300, 400, 56 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );
    fill_gpu_boxes( one( -100, -100, 300, 300, 56 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 );
    CHECK_NEAR( t, boxes.d[0].x0, 0.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y0, 80.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].x1, 100.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y1, 180.0f, 1e-4f );

    // Thinner than 2 px in the tensor: rejected.
    fill_gpu_boxes( one( 100, 200, 2, 400, 56 ), no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // A tentative track draws its box (person red) and no trail.
    {
        TrajList trajs;
        trajs.push( TrackState { 1, TRACK_TENT, 640.0f, 480.0f, 0.0f, 0.0f, 100.0f, 200.0f, 0 } );
        fill_gpu_boxes( DetList {}, trajs, idle, boxes, kBand, kScale, kScale, 0 );
        CHECK( t, boxes.n == 1 && color( boxes.d[0], 255, 64, 64 ) && boxes.d[0].a == 255 );
        CHECK_NEAR( t, boxes.d[0].x0, 295.0f, 1e-4f );
        CHECK_NEAR( t, boxes.d[0].y0, 270.0f, 1e-4f );
        CHECK_NEAR( t, boxes.d[0].x1, 345.0f, 1e-4f );
        CHECK_NEAR( t, boxes.d[0].y1, 370.0f, 1e-4f );
    }

    // A confirmed walking track: its box first, then 3x3 trail dots in the
    // track color, alpha non-decreasing from the oldest to the newest dot.
    {
        Tracker trk;
        TrajList trajs;
        int64_t ts = 0;
        float cx = 200.0f;
        for ( int i = 0; i < 100; ++i, ts += 40000, cx += 4.0f )
        {
            DetList dets;
            dets.push( Detection { cx - 50.0f, 300.0f, 100.0f, 200.0f, 0, 0.9f } );
            trk.update( dets, ts, trajs );
        }
        const int64_t now = ts - 40000;
        fill_gpu_boxes( DetList {}, trajs, trk, boxes, kBand, kScale, kScale, now );
        CHECK( t, boxes.n > 3 );
        CHECK( t, color( boxes.d[0], 255, 64, 64 ) );  // the track box
        bool dots_ok = true;
        bool alpha_ok = true;
        for ( int k = 1; k < boxes.n; ++k )
        {
            const GpuBox& p = boxes.d[k];
            dots_ok = dots_ok && color( p, 0, 255, 255 )  // palette entry for id 1
                      && std::fabs( ( p.x1 - p.x0 ) - 3.0f ) < 1e-3f && std::fabs( ( p.y1 - p.y0 ) - 3.0f ) < 1e-3f;
            alpha_ok = alpha_ok && p.a > 0 && ( k == 1 || p.a >= boxes.d[k - 1].a );
        }
        CHECK( t, dots_ok );
        CHECK( t, alpha_ok );

        // The ghost: no box once the track is dead, the trail keeps drawing.
        for ( int miss = 0; miss < 5; ++miss, ts += 40000 )
            trk.update( DetList {}, ts, trajs );
        CHECK( t, trajs.n == 1 && trajs.d[0].state == TRACK_DEAD );
        fill_gpu_boxes( DetList {}, trajs, trk, boxes, kBand, kScale, kScale, ts - 40000 );
        CHECK( t, boxes.n > 1 );
        CHECK( t, color( boxes.d[0], 0, 255, 255 ) );  // dots only
    }

    // The 128-box budget is a hard cap.
    {
        DetList many;
        for ( int i = 0; i < 128; ++i )
            many.push( Detection { 10.0f * static_cast<float>( i ), 200.0f, 8.0f, 8.0f, 56, 0.9f } );
        TrajList trajs;
        trajs.push( TrackState { 1, TRACK_TENT, 640.0f, 480.0f, 0.0f, 0.0f, 100.0f, 200.0f, 0 } );
        fill_gpu_boxes( many, trajs, idle, boxes, kBand, kScale, kScale, 0 );
        CHECK( t, boxes.n == 128 );
    }
}
