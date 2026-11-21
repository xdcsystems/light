#include <cmath>
#include <sstream>

#include "render/box_smoother.hpp"
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
    // A filtered value: from the track's previous box `from` toward the new raw
    // box `to`, but not all the way (the dead zone may keep it at from). A
    // pass-through would be exactly `to`.
    bool toward( float v, float from, float to )
    {
        return ( v >= from && v < to ) || ( v <= from && v > to );
    }

    void test_filter_and_coast( TestRun& t )
    {
        // A standing jittering box (+-3 px) settles and then stays exactly still
        // (BoxFilter's dead zone); a real move makes it follow at once.
        {
            BoxSmoother sm;
            DetList out;
            const float dx[8] = { -2.0f, 3.0f, -1.0f, 2.0f, -3.0f, 1.0f, 2.0f, -2.0f };
            for ( int k = 0; k < 40; ++k )
                sm.update( one( det( 200.0f + dx[k % 8], 150.0f - dx[k % 8], 80.0f, 120.0f, 56 ) ), out );
            CHECK( t, out.n == 1 );
            const float held_x = out.d[0].x;
            const float held_y = out.d[0].y;
            CHECK_NEAR( t, held_x, 200.0f, 3.0f );
            CHECK_NEAR( t, held_y, 150.0f, 3.0f );
            for ( int k = 0; k < 40; ++k )
            {
                sm.update( one( det( 200.0f + dx[k % 8], 150.0f - dx[k % 8], 80.0f, 120.0f, 56 ) ), out );
                CHECK( t, out.d[0].x == held_x && out.d[0].y == held_y );
            }
            sm.update( one( det( 230.0f, 150.0f, 80.0f, 120.0f, 56 ) ), out );
            CHECK( t, out.d[0].x > held_x );
        }
        // A continuously moving box never freezes.
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
        // Confirmed track (hits >= 2) coasts unchanged across 6 missed frames and expires on the 7th.
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
                CHECK( t, out.d[0].x == held_x );
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
            sm.update( one( Detection { 310, 200, 40, 80, 75, 0.29f } ), out );
            CHECK( t, out.n == 1 );
            CHECK( t, toward( out.d[0].x, 300.0f, 310.0f ) );
            const float shown = out.d[0].x;
            sm.update( DetList {}, out );
            CHECK( t, out.n == 1 && out.d[0].x == shown );
        }
    }

    // A slow walk (spd px/frame) from a standing start: the box never moves
    // backwards, never faster than 1.15x the walk once it moves, and keeps up.
    void check_slow_ramp( TestRun& t, float spd )
    {
        BoxSmoother sm;
        DetList out;
        for ( int k = 0; k < 20; ++k )
            sm.update( one( det( 100.0f, 150.0f, 80.0f, 120.0f, 56 ) ), out );
        float prev_x = out.d[0].x;
        float x = 100.0f;
        for ( int k = 1; k <= 60; ++k )
        {
            x = 100.0f + static_cast<float>( k ) * spd;
            sm.update( one( det( x, 150.0f, 80.0f, 120.0f, 56 ) ), out );
            const float dx = out.d[0].x - prev_x;
            CHECK( t, dx >= 0.0f && dx <= 1.15f * spd + 1e-3f );
            prev_x = out.d[0].x;
        }
        CHECK( t, x - out.d[0].x < 10.0f );
    }

    void test_slow_ramp( TestRun& t )
    {
        const float speeds[4] = { 0.5f, 1.0f, 2.0f, 3.0f };
        for ( const float spd : speeds )
            check_slow_ramp( t, spd );
    }
}  // namespace

void test_box_smoother( TestRun& t )
{
    // M-14: a NaN or empty non-person det is dropped -- not spawned, not
    // emitted -- and does not disturb a live smoothed box.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( std::nanf( "" ), 10.0f, 80.0f, 120.0f, 56 ) ), out );
        CHECK( t, out.n == 0 );
        sm.update( one( det( 10.0f, 10.0f, 0.0f, 120.0f, 56 ) ), out );
        CHECK( t, out.n == 0 );
        for ( int i = 0; i < 3; ++i )
            sm.update( one( det( 200.0f, 150.0f, 80.0f, 120.0f, 56 ) ), out );
        DetList mixed;
        mixed.push( det( 200.0f, 150.0f, 80.0f, 120.0f, 56 ) );
        mixed.push( det( 200.0f, std::nanf( "" ), 80.0f, 120.0f, 56 ) );
        sm.update( mixed, out );
        CHECK( t, out.n == 1 && std::isfinite( out.d[0].y ) );
        CHECK_NEAR( t, out.d[0].x, 200.0f, 1e-3f );
    }
    // First sighting of non-person passes through unsmoothed; the next match is filtered.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 2 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK_NEAR( t, out.d[0].x, 100.0f, 1e-4f );
        sm.update( one( det( 110, 104, 54, 84, 2 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK( t, toward( out.d[0].x, 100.0f, 110.0f ) );
        CHECK( t, toward( out.d[0].y, 100.0f, 104.0f ) );
        CHECK( t, toward( out.d[0].w, 50.0f, 54.0f ) );
        CHECK( t, toward( out.d[0].h, 80.0f, 84.0f ) );
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
        CHECK( t, toward( out.d[1].x, 100.0f, 108.0f ) );  // cls == 56 got a slot despite 40 people
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
        CHECK( t, toward( out.d[0].x, 100.0f, 110.0f ) );  // still tracked: smoothed

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
        CHECK( t, toward( out.d[1].x, 100.0f, 104.0f ) );  // matched: smoothed
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
        const DetList out;
        sm.log_frame( 0, one( det( 1, 1, 1, 1, 0 ) ), out );  // no CSV open: a no-op
        CHECK( t, lout.str().empty() );
    }
    test_filter_and_coast( t );
    test_slow_ramp( t );
}
