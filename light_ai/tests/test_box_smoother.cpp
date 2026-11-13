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
}  // namespace

void test_box_smoother( TestRun& t )
{
    // First sighting passes through unsmoothed; the next match is the EMA
    // (alpha 0.5 = plain average of track and detection).
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK_NEAR( t, out.d[0].x, 100.0f, 1e-4f );
        sm.update( one( det( 110, 104, 54, 84, 0 ) ), out );
        CHECK( t, out.n == 1 );
        CHECK_NEAR( t, out.d[0].x, 105.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].y, 102.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].w, 52.0f, 1e-4f );
        CHECK_NEAR( t, out.d[0].h, 82.0f, 1e-4f );
        CHECK( t, out.d[0].cls == 0 );
        CHECK_NEAR( t, out.d[0].conf, 0.9f, 1e-6f );  // only the box is filtered
    }
    // Preallocated buffer contract: out is cleared at entry, never accumulated.
    {
        BoxSmoother sm;
        DetList out;
        out.push( det( 1, 2, 3, 4, 5 ) );
        out.push( det( 1, 2, 3, 4, 5 ) );
        sm.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        CHECK( t, out.n == 1 );
        sm.update( DetList {}, out );
        CHECK( t, out.n == 0 );
    }
    // A different class never matches an existing track: pass-through.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        sm.update( one( det( 110, 100, 50, 80, 56 ) ), out );
        CHECK( t, out.n == 1 && out.d[0].cls == 56 );
        CHECK_NEAR( t, out.d[0].x, 110.0f, 1e-4f );
    }
    // IoU at or below the 0.30 gate is not a match: pass-through.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        sm.update( one( det( 140, 100, 50, 80, 0 ) ), out );  // IoU = 10/90
        CHECK_NEAR( t, out.d[0].x, 140.0f, 1e-4f );
    }
    // A track survives 2 missed frames and is freed on the 3rd.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        sm.update( DetList {}, out );
        sm.update( DetList {}, out );
        sm.update( one( det( 110, 100, 50, 80, 0 ) ), out );
        CHECK_NEAR( t, out.d[0].x, 105.0f, 1e-4f );  // still tracked: smoothed

        BoxSmoother gone;
        gone.update( one( det( 100, 100, 50, 80, 0 ) ), out );
        gone.update( DetList {}, out );
        gone.update( DetList {}, out );
        gone.update( DetList {}, out );
        gone.update( one( det( 110, 100, 50, 80, 0 ) ), out );
        CHECK_NEAR( t, out.d[0].x, 110.0f, 1e-4f );  // track expired: fresh pass-through
    }
    // Output keeps the input order; the better IoU wins the shared track.
    {
        BoxSmoother sm;
        DetList out;
        sm.update( one( det( 100, 100, 100, 100, 0 ) ), out );
        DetList two;
        two.push( det( 130, 100, 100, 100, 0 ) );  // weaker overlap, listed first
        two.push( det( 104, 100, 100, 100, 0 ) );  // stronger overlap
        sm.update( two, out );
        CHECK( t, out.n == 2 );
        CHECK_NEAR( t, out.d[0].x, 130.0f, 1e-4f );  // lost the track: passes through
        CHECK_NEAR( t, out.d[1].x, 102.0f, 1e-4f );  // matched: smoothed
    }
}
