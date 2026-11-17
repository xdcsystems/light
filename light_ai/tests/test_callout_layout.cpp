#include <algorithm>
#include <string>

#include "check.hpp"
#include "core/callout.hpp"
#include "suites.hpp"

namespace
{
    int overlap_area( const CalloutRect& a, const CalloutRect& b ) noexcept
    {
        const int ox = std::min( a.x1, b.x1 ) - std::max( a.x0, b.x0 );
        const int oy = std::min( a.y1, b.y1 ) - std::max( a.y0, b.y0 );
        return ( ox > 0 && oy > 0 ) ? ( ox * oy ) : 0;
    }

    bool inside_even( const CalloutRect& r, const LbGeom& lb ) noexcept
    {
        const int cl = ( lb.left & ~1 ) + 2;
        const int ct = ( lb.top & ~1 ) + 2;
        const int cr = ( lb.left & ~1 ) + ( lb.w & ~1 ) - 2;
        const int cb = ( lb.top & ~1 ) + ( lb.h & ~1 ) - 2;
        return r.x0 >= cl && r.y0 >= ct && r.x1 <= cr && r.y1 <= cb && r.x1 > r.x0 && r.y1 > r.y0;
    }

}  // namespace

void test_callout_layout( TestRun& t )
{
    const LbGeom lb { 0, 640, 140, 360 };
    const float sx = 640.0f / 1280.0f;
    const float sy = 360.0f / 720.0f;

    // 1. Dead track ignored; tentative + confirmed included; global_id overrides display id.
    {
        TrajList trajs {};
        trajs.push( TrackState { 3, TRACK_DEAD, 300.0f, 300.0f, 0.0f, 0.0f, 80.0f, 160.0f, 5 } );
        TrackState t1 { 2, TRACK_TENT, 400.0f, 320.0f, 0.0f, 0.0f, 90.0f, 180.0f, 1, 99 };
        trajs.push( t1 );
        CalloutList out {};
        compute_callouts( trajs, lb, sx, sy, { false }, out );
        CHECK( t, out.n == 1 );
        CHECK( t, std::string( out.d[0].text ) == "person 99" );
        CHECK( t, inside_even( out.d[0].pill, lb ) );
    }

    // 2. Russian label + up to 8 clustered people -> all pills stay inside band and non-overlapping.
    {
        TrajList trajs {};
        for ( int i = 0; i < 8; ++i )
        {
            const float cx = 260.0f + static_cast<float>( i % 4 ) * 190.0f;
            const int row = i / 4;
            const float cy = 220.0f + static_cast<float>( row ) * 220.0f;
            trajs.push( TrackState { i + 1, TRACK_CONF, cx, cy, 0.0f, 0.0f, 100.0f, 180.0f, 4 } );
        }
        CalloutList out {};
        compute_callouts( trajs, lb, sx, sy, { true }, out );
        CHECK( t, out.n == 8 );
        bool pills_ok = true;
        for ( int i = 0; i < out.n; ++i )
        {
            pills_ok = pills_ok && inside_even( out.d[i].pill, lb );
            for ( int j = i + 1; j < out.n; ++j )
                pills_ok = pills_ok && ( overlap_area( out.d[i].pill, out.d[j].pill ) == 0 );
        }
        CHECK( t, pills_ok );
    }

    // 3. Tiny degenerate letterbox band narrower than pill width skips cleanly (no std::clamp UB).
    {
        const LbGeom tiny { 10, 40, 20, 20 };
        TrajList trajs {};
        trajs.push( TrackState { 1, TRACK_CONF, 50.0f, 50.0f, 0.0f, 0.0f, 30.0f, 30.0f, 2 } );
        CalloutList out {};
        compute_callouts( trajs, tiny, 0.1f, 0.1f, { true }, out );
        CHECK( t, out.n == 0 );
    }

    // 4. Odd lb coordinates align to FrameComposer even crop + top-edge tracks clamp cleanly.
    {
        const LbGeom odd { 3, 637, 15, 363 };
        TrajList trajs {};
        trajs.push( TrackState { 5, TRACK_CONF, 20.0f, 10.0f, 0.0f, 0.0f, 40.0f, 40.0f, 2 } );
        CalloutList out {};
        compute_callouts( trajs, odd, 0.5f, 0.5f, { false }, out );
        CHECK( t, out.n == 1 && inside_even( out.d[0].pill, odd ) );
    }

    // 5. Hysteresis prevents side flip-flop across 1-px width/center jitter when prior side fits.
    {
        CalloutHistory hist {};
        // Box sits near top edge so top candidates clip/overlap own box; sides (6 right, 7 left) are open.
        const float xs[4] = { 640.0f, 641.0f, 639.0f, 642.0f };
        const float ws[4] = { 100.0f, 101.0f, 99.0f, 102.0f };
        bool stayed_on_side = true;
        int first_side = 0;
        for ( int step = 0; step < 4; ++step )
        {
            TrajList trajs {};
            trajs.push( TrackState { 1, TRACK_CONF, xs[step], 110.0f, 0.0f, 0.0f, ws[step], 200.0f, 4 } );
            CalloutList out {};
            compute_callouts( trajs, lb, sx, sy, { false }, hist, out );
            CHECK( t, out.n == 1 );
            const int side = ( out.d[0].pill.x0 >= out.d[0].box.x1 ) ? 1 : -1;
            if ( step == 0 )
                first_side = side;
            else
                stayed_on_side = stayed_on_side && ( side == first_side );
        }
        CHECK( t, first_side == 1 && stayed_on_side );

        // Pushing box to right boundary moves pill cleanly to the left and holds it there.
        TrajList edge_tr {};
        edge_tr.push( TrackState { 1, TRACK_CONF, 1200.0f, 110.0f, 0.0f, 0.0f, 120.0f, 200.0f, 5 } );
        CalloutList out_edge {};
        compute_callouts( edge_tr, lb, sx, sy, { false }, hist, out_edge );
        CHECK( t, out_edge.n == 1 && out_edge.d[0].pill.x1 <= out_edge.d[0].box.x0 );
    }
}
