#include <sstream>

#include "box_smoother.h"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    Detection det( float x, float y, float w, float h, int cls )
    {
        return Detection { x, y, w, h, cls, 0.9f };
    }

    DetList one( const Detection& d )
    {
        DetList l;
        l.push( d );
        return l;
    }
    void test_envelope_and_coast( TestRun& t )
    {
        // Stationary jittering box calibrates [lo, hi] over window, locks onto mean (0 px delta),
        // and breaks out immediately when detections move outside the bounds.
        {
            BoxSmoother sm;
            DetList out;
            const float dx[8] = { -2.0f, 3.0f, -1.0f, 2.0f, -3.0f, 1.0f, 2.0f, -2.0f };
            for ( const float off : dx )
                sm.update( one( det( 200.0f + off, 150.0f - off, 80.0f, 120.0f, 56 ) ), out );
            CHECK( t, out.n == 1 );
            const float locked_x = out.d[0].x;
            const float locked_y = out.d[0].y;
            CHECK_NEAR( t, locked_x, 200.0f, 1e-4f );
            CHECK_NEAR( t, locked_y, 150.0f, 1e-4f );

            // Subsequent jitter inside [lo - pad, hi + pad] holds the locked mean identically.
            sm.update( one( det( 203.0f, 148.0f, 80.0f, 120.0f, 56 ) ), out );
            CHECK_NEAR( t, out.d[0].x, locked_x, 1e-6f );
            CHECK_NEAR( t, out.d[0].y, locked_y, 1e-6f );
            sm.update( one( det( 197.0f, 152.0f, 80.0f, 120.0f, 56 ) ), out );
            CHECK_NEAR( t, out.d[0].x, locked_x, 1e-6f );
            CHECK_NEAR( t, out.d[0].y, locked_y, 1e-6f );

            // Moving outside bounds unlocks right away without lag.
            sm.update( one( det( 230.0f, 150.0f, 80.0f, 120.0f, 56 ) ), out );
            CHECK( t, out.d[0].x > 210.0f );

            // Steady frames at the new position re-lock cleanly on the new center.
            for ( int k = 0; k < BoxEnvelope::kWindowFrames; ++k )
                sm.update( one( det( 230.0f + ( k & 1 ? 1.0f : -1.0f ), 150.0f, 80.0f, 120.0f, 56 ) ), out );
            const float relocked_x = out.d[0].x;
            sm.update( one( det( 231.0f, 150.0f, 80.0f, 120.0f, 56 ) ), out );
            CHECK_NEAR( t, out.d[0].x, relocked_x, 1e-6f );
        }
        // Continuously moving box exceeds stationary span and never freezes.
        {
            BoxSmoother sm;
            DetList out;
            float prev_x = 0.0f;
            for ( int k = 0; k < 12; ++k )
            {
                sm.update( one( det( 50.0f + static_cast<float>( k ) * 20.0f, 100.0f, 60.0f, 90.0f, 56 ) ), out );
                if ( k > 0 )
                    CHECK( t, out.d[0].x > prev_x );
                prev_x = out.d[0].x;
            }
        }
        // Confirmed track (hits >= 2) coasts smoothly across 6 missed frames and expires on the 7th.
        {
            BoxSmoother sm;
            DetList out;
            sm.update( one( det( 120, 140, 60, 90, 58 ) ), out );
            sm.update( one( det( 122, 140, 60, 90, 58 ) ), out );
            CHECK( t, out.n == 1 );
            const float held_x = out.d[0].x;
            for ( int m = 0; m < 6; ++m )
            {
                sm.update( DetList {}, out );
                CHECK( t, out.n == 1 && out.d[0].cls == 58 );
                CHECK_NEAR( t, out.d[0].x, held_x, 1e-4f );
            }
            sm.update( DetList {}, out );
            CHECK( t, out.n == 0 );
        }
        // Weak detection (< 0.35) does not flash on 1 frame, appears once confirmed, then coasts on drop.
        {
            BoxSmoother sm;
            DetList out;
            sm.update( one( Detection { 300, 200, 40, 80, 75, 0.28f } ), out );
            CHECK( t, out.n == 0 );
            sm.update( one( Detection { 302, 200, 40, 80, 75, 0.29f } ), out );
            CHECK( t, out.n == 1 );
            CHECK_NEAR( t, out.d[0].x, 301.0f, 1e-4f );
            sm.update( DetList {}, out );
            CHECK( t, out.n == 1 );
            CHECK_NEAR( t, out.d[0].x, 301.0f, 1e-4f );
        }
    }

    void check_slow_ramp_walk( TestRun& t, float spd )
    {
        BoxSmoother sm;
        DetList out;
        float prev_x = 0.0f;
        for ( int k = 0; k < 30; ++k )
        {
            sm.update( one( det( 100.0f + static_cast<float>( k ) * spd, 150.0f, 80.0f, 120.0f, 56 ) ), out );
            if ( k > 0 )
            {
                const float dx = out.d[0].x - prev_x;
                CHECK( t, dx > 0.0f && dx <= 1.5f * spd + 1e-3f );
            }
            prev_x = out.d[0].x;
        }

        // Direct OverlaySink path (move_alpha = 0): steady slow walk from start never freezes or jerks.
        BoxEnvelope walk;
        float prev_wx = 0.0f;
        for ( int k = 0; k < 30; ++k )
        {
            const float shift = static_cast<float>( k ) * spd;
            walk.step( 100.0f + shift, 100.0f, 180.0f + shift, 220.0f, 0.0f );
            if ( k > 0 )
            {
                const float dx = walk.cx() - prev_wx;
                CHECK( t, dx > 0.0f && dx <= 1.5f * spd + 1e-3f );
            }
            prev_wx = walk.cx();
        }
    }

    void check_slow_ramp_breakout( TestRun& t, float spd )
    {
        // Stationary lock -> slow ramp breaks out cleanly and stays smooth thereafter.
        BoxEnvelope env;
        for ( int k = 0; k < BoxEnvelope::kWindowFrames; ++k )
            env.step( 100.0f, 100.0f, 180.0f, 220.0f, 0.0f );
        float prev_cx = env.cx();
        int frozen = 0;
        for ( int k = 1; k <= 40; ++k )
        {
            const float shift = static_cast<float>( k ) * spd;
            env.step( 100.0f + shift, 100.0f, 180.0f + shift, 220.0f, 0.0f );
            const float cx = env.cx();
            const float dx = cx - prev_cx;
            if ( k >= 14 )
            {
                frozen += ( dx <= 1e-5f ) ? 1 : 0;
                CHECK( t, dx <= 1.5f * spd + 1e-3f );
            }
            prev_cx = cx;
        }
        CHECK( t, frozen == 0 );
    }

    void test_slow_ramp( TestRun& t )
    {
        const float speeds[4] = { 0.5f, 1.0f, 2.0f, 3.0f };
        for ( float spd : speeds )
        {
            check_slow_ramp_walk( t, spd );
            check_slow_ramp_breakout( t, spd );
        }
    }
}  // namespace

void test_box_smoother( TestRun& t )
{
    // First sighting of non-person passes through unsmoothed; next match is EMA (alpha 0.5).
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK_NEAR( t, out.d[0].x, 100.0f, 1e-4f );
        sm.update( one( det( 110, 104, 54, 84, 2 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK_NEAR( t, out.d[0].x, 105.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].y, 102.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].w, 52.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].h, 82.0f, 1e-4f );
        CHECK( t, out.d[0].cls == 2 );
        CHECK_NEAR( t, out.d[0].conf, 0.9f, 1e-6f );  // only the box is filtered
    }
    // Persons (cls == 0) pass through unsmoothed and do not consume the 32 smoother slots.
    {
        BoxSmoother sm;
        DetList crowd;
        for ( int i = 0; i < 40; ++i )
            crowd.push( det( 10.0f + static_cast<float>( i ) * 25.0f, 100.0f, 20.0f, 60.0f, 0 ) );
        crowd.push( det( 100.0f, 300.0f, 50.0f, 80.0f, 56 ) );
        DetList out;
        sm.update( crowd, out );
        CHECK( t, out.n == 41 );
        DetList next;
        next.push( det( 14.0f, 100.0f, 20.0f, 60.0f, 0 ) );
        next.push( det( 108.0f, 300.0f, 50.0f, 80.0f, 56 ) );
        sm.update( next, out );
        CHECK( t, out.n == 2 );
        CHECK_NEAR( t, out.d[0].x, 14.0f, 1e-4f );   // cls == 0 unsmoothed pass-through
        CHECK_NEAR( t, out.d[1].x, 104.0f, 1e-4f );  // cls == 56 got a slot despite 40 people
    }
    // Preallocated buffer contract: out is cleared at entry, never accumulated.
    {
        BoxSmoother sm;
        DetList out;
        out.push( det( 1, 2, 3, 4, 5 ) );
        out.push( det( 1, 2, 3, 4, 5 ) );
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        CHECK( t, out.n == 1 );
        sm.update( DetList {}, out );
        CHECK( t, out.n == 0 );
    }
    // A different class never matches an existing track: pass-through.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        sm.update( one( det( 110, 100, 50, 80, 56 ) ), out );
        CHECK( t, out.n == 1 && out.d[0].cls == 56 );
        CHECK_NEAR( t, out.d[0].x, 110.0f, 1e-4f );
    }
    // IoU at or below the 0.30 gate is not a match: pass-through.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        sm.update( one( det( 140, 100, 50, 80, 2 ) ), out );  // IoU = 10/90
        CHECK_NEAR( t, out.d[0].x, 140.0f, 1e-4f );
    }
    // A track survives 2 missed frames and is freed on the 3rd.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        sm.update( DetList {}, out );
        sm.update( DetList {}, out );
        sm.update( one( det( 110, 100, 50, 80, 2 ) ), out );
        CHECK_NEAR( t, out.d[0].x, 105.0f, 1e-4f );  // still tracked: smoothed

        BoxSmoother gone;
        gone.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        gone.update( DetList {}, out );
        gone.update( DetList {}, out );
        gone.update( DetList {}, out );
        gone.update( one( det( 110, 100, 50, 80, 2 ) ), out );
        CHECK_NEAR( t, out.d[0].x, 110.0f, 1e-4f );  // track expired: fresh pass-through
    }
    // Output keeps the input order; the better IoU wins the shared track.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 100, 100, 2 ) ), out );
        DetList two;
        two.push( det( 130, 100, 100, 100, 2 ) );  // weaker overlap, listed first
        two.push( det( 104, 100, 100, 100, 2 ) );  // stronger overlap
        sm.update( two, out );
        CHECK( t, out.n == 2 );
        CHECK_NEAR( t, out.d[0].x, 130.0f, 1e-4f );  // lost the track: passes through
        CHECK_NEAR( t, out.d[1].x, 102.0f, 1e-4f );  // matched: smoothed
    }
    // set_log: an empty path is silent; an unopenable one is reported through
    // the Log (warn) and the CSV stays off.
    {
        std::ostringstream lout;
        std::ostringstream lerr;
        const Log log( lout, lerr );
        BoxSmoother sm;
        sm.set_log( "", log );
        CHECK( t, lerr.str().empty() );
        sm.set_log( "/nonexistent-dir/box.csv", log );
        CHECK( t, lerr.str() == "smoother: box-log open failed for '/nonexistent-dir/box.csv' - A/B logging disabled\n" );
        DetList out;
        sm.log_frame( 0, one( det( 1, 1, 1, 1, 0 ) ), out );  // no CSV open: a no-op
        CHECK( t, lout.str().empty() );
    }
    test_envelope_and_coast( t );
    test_slow_ramp( t );
}
