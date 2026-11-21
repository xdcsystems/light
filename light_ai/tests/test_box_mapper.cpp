#include <cmath>
#include <cstdint>

#include "render/box_mapper.hpp"
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

    void check_m4_m6_color_and_priority( TestRun& t )
    {
        BoxMapper mapper;
        GpuBoxList boxes;

        // [M-4]: trail_color follows display_id() when global_id > 0 (palette[5] = {200, 0, 0}).
        {
            Tracker trk;
            TrajList trajs;
            int64_t ts = 0;
            for ( int i = 0; i < 15; ++i, ts += 40000 )
            {
                DetList dets;
                dets.push( Detection { 200.0f + 10.0f * static_cast<float>( i ), 300.0f, 100.0f, 200.0f, 0, 0.9f } );
                trk.update( dets, ts, trajs );
            }
            CHECK( t, trajs.n == 1 && trajs.d[0].id == 1 );
            trajs.d[0].global_id = 5;
            mapper.fill( DetList {}, trajs, trajs, trk, boxes, kBand, kScale, kScale, ts - 40000 );
            CHECK( t, boxes.n > 1 );
            CHECK( t, color( boxes.d[0], 255, 64, 64 ) );
            CHECK( t, color( boxes.d[1], 200, 0, 0 ) );
        }

        // [M-6]: 8 tracks x >20 trail dots saturates 128-cap, yet all 8 track_boxes emit first.
        {
            Tracker trk;
            TrajList trajs;
            int64_t ts = 0;
            for ( int f = 0; f < 30; ++f, ts += 40000 )
            {
                DetList dets;
                for ( int k = 0; k < 8; ++k )
                {
                    const float x = 60.0f + static_cast<float>( k ) * 140.0f + 8.0f * static_cast<float>( f );
                    dets.push( Detection { x, 200.0f, 60.0f, 140.0f, 0, 0.9f } );
                }
                trk.update( dets, ts, trajs );
            }
            CHECK( t, trajs.n == 8 );
            mapper.fill( DetList {}, trajs, trajs, trk, boxes, kBand, kScale, kScale, ts - 40000 );
            CHECK( t, boxes.n == 128 );
            bool all_boxes_first = true;
            for ( int i = 0; i < 8; ++i )
                all_boxes_first = all_boxes_first && color( boxes.d[i], 255, 64, 64 ) && boxes.d[i].a == 255;
            CHECK( t, all_boxes_first );
        }
    }

    // #91 item 4: the display box (BoxFilter output) and the trail meet. With the
    // display track shifted off its posterior, the newest trail dot moves by the
    // full shift, the shift fades out over kTrailBlend dots, older dots stay put;
    // a ghost (DEAD) never bends; a post list of another track does not bend either.
    void check_trail_head_follows_box( TestRun& t )
    {
        Tracker trk;
        TrajList post;
        int64_t ts = 0;
        // 12 scene px a frame: the dots stand 6 tensor px apart, so a 3 px bend
        // never merges two of them (the 4 px dot dedup of trail_dot).
        for ( int i = 0; i < 40; ++i, ts += 40000 )
        {
            DetList dets;
            dets.push( Detection { 150.0f + 12.0f * static_cast<float>( i ), 300.0f, 100.0f, 200.0f, 0, 0.9f } );
            trk.update( dets, ts, post );
        }
        const int64_t now = ts - 40000;
        CHECK( t, post.n == 1 && post.d[0].state == TRACK_CONF );
        BoxMapper mapper;
        GpuBoxList straight;
        mapper.fill( DetList {}, post, post, trk, straight, kBand, kScale, kScale, now );
        TrajList disp = post;
        disp.d[0].cx -= 6.0f;  // the display box lags 6 scene px = 3 tensor px behind the posterior
        GpuBoxList bent;
        mapper.fill( DetList {}, disp, post, trk, bent, kBand, kScale, kScale, now );
        CHECK( t, straight.n == bent.n && straight.n > BoxMapper::kTrailBlend + 2 );
        if ( straight.n != bent.n || straight.n <= BoxMapper::kTrailBlend + 2 )
            return;
        const int newest = bent.n - 1;
        CHECK_NEAR( t, bent.d[0].x0 - straight.d[0].x0, -3.0f, 1e-3f );  // the box itself moved
        CHECK_NEAR( t, bent.d[newest].x0 - straight.d[newest].x0, -3.0f, 1e-3f );
        bool fades = true;
        for ( int j = 1; j < newest; ++j )
        {
            const float shift = bent.d[j].x0 - straight.d[j].x0;
            const float expect = -3.0f * std::fmax( 0.0f, 1.0f - static_cast<float>( newest - j ) / BoxMapper::kTrailBlend );
            fades = fades && std::fabs( shift - expect ) < 1e-3f && bent.d[j].y0 == straight.d[j].y0;
        }
        CHECK( t, fades );
        // A post entry of another track (id mismatch): no bend.
        TrajList other = post;
        other.d[0].id += 1;
        GpuBoxList unbent;
        mapper.fill( DetList {}, disp, other, trk, unbent, kBand, kScale, kScale, now );
        CHECK( t, unbent.n == straight.n && unbent.d[newest].x0 == straight.d[newest].x0 );
        // A ghost: no box, the trail stays where it was recorded.
        TrajList ghost_disp = disp;
        TrajList ghost_post = post;
        ghost_disp.d[0].state = TRACK_DEAD;
        ghost_post.d[0].state = TRACK_DEAD;
        GpuBoxList ghost;
        mapper.fill( DetList {}, ghost_disp, ghost_post, trk, ghost, kBand, kScale, kScale, now );
        CHECK( t, ghost.n == straight.n - 1 && ghost.d[ghost.n - 1].x0 == straight.d[newest].x0 );
    }
}  // namespace

void test_box_mapper( TestRun& t )
{
    const Tracker idle;  // no tracks: recent_points() serves nothing
    const TrajList no_tracks;
    GpuBoxList boxes;
    BoxMapper mapper;

    // A non-person detection maps scene -> tensor: scale, then the band offset.
    // M-14: NaN / Inf / empty geometry never becomes a box.
    mapper.fill( one( std::nanf( "" ), 200, 300, 400, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );
    mapper.fill( one( 100, 200, INFINITY, 400, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );
    mapper.fill( one( 100, 200, 300, 0, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    mapper.fill( one( 100, 200, 300, 400, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 );
    CHECK_NEAR( t, boxes.d[0].x0, 50.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y0, 180.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].x1, 200.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y1, 380.0f, 1e-4f );
    CHECK( t, color( boxes.d[0], 64, 255, 64 ) && boxes.d[0].a == 255 );  // chair

    mapper.fill( one( 100, 200, 300, 400, 2 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 && color( boxes.d[0], 255, 255, 0 ) );  // any other class

    // Preallocated buffer contract: boxes is cleared at entry.
    mapper.fill( DetList {}, no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // Persons are drawn from the track list only, never from the detection list.
    mapper.fill( one( 100, 200, 300, 400, 0 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // A box fully outside the band is rejected; a partial one is clamped to the band.
    mapper.fill( one( 2000, 200, 300, 400, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );
    mapper.fill( one( -100, -100, 300, 300, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 1 );
    CHECK_NEAR( t, boxes.d[0].x0, 0.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y0, 80.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].x1, 100.0f, 1e-4f );
    CHECK_NEAR( t, boxes.d[0].y1, 180.0f, 1e-4f );

    // Thinner than 2 px in the tensor: rejected.
    mapper.fill( one( 100, 200, 2, 400, 56 ), no_tracks, no_tracks, idle, boxes, kBand, kScale, kScale, 0 );
    CHECK( t, boxes.n == 0 );

    // A tentative track draws its box (person red) and no trail.
    {
        TrajList trajs;
        trajs.push( TrackState { 1, TRACK_TENT, 640.0f, 480.0f, 0.0f, 0.0f, 100.0f, 200.0f, 0 } );
        mapper.fill( DetList {}, trajs, trajs, idle, boxes, kBand, kScale, kScale, 0 );
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
        for ( int i = 0; i < 100; ++i, ts += 40000 )
        {
            const float cx = 200.0f + 4.0f * static_cast<float>( i );
            DetList dets;
            dets.push( Detection { cx - 50.0f, 300.0f, 100.0f, 200.0f, 0, 0.9f } );
            trk.update( dets, ts, trajs );
        }
        const int64_t now = ts - 40000;
        mapper.fill( DetList {}, trajs, trajs, trk, boxes, kBand, kScale, kScale, now );
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

        // The ghost: no box once the track is dead (dies on 7th miss for kMaxMiss=6), the trail keeps drawing.
        for ( int miss = 0; miss < 7; ++miss, ts += 40000 )
            trk.update( DetList {}, ts, trajs );
        CHECK( t, trajs.n == 1 && trajs.d[0].state == TRACK_DEAD );
        mapper.fill( DetList {}, trajs, trajs, trk, boxes, kBand, kScale, kScale, ts - 40000 );
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
        mapper.fill( many, trajs, trajs, idle, boxes, kBand, kScale, kScale, 0 );
        CHECK( t, boxes.n == 128 );
    }
    check_m4_m6_color_and_priority( t );
    check_trail_head_follows_box( t );
}
