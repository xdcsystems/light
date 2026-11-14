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
        // A new session: a new base, and the monotonic clamp starts over.
        c.restart( 1 );
        CHECK( t, c.next( 9000000, 11 ) == 0 );
        CHECK( t, c.next( 9010000, 12 ) == 900 );
        CHECK( t, c.packet_ts( 900 ) == 900 );
    }
}  // namespace

void test_pts_clock( TestRun& t )
{
    rescale( t );
    cfr( t );
    vfr( t );
}
