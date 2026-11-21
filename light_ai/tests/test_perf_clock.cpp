#include <chrono>
#include <cstdint>
#include <limits>

#include "check.hpp"
#include "core/perf_clock.hpp"
#include "suites.hpp"

namespace
{
    PerfClock::time_point at_us( int64_t us )
    {
        return PerfClock::time_point( std::chrono::microseconds( us ) );
    }

    void elapsed( TestRun& t )
    {
        CHECK_NEAR( t, ms_between( at_us( 1000 ), at_us( 3500 ) ), 2.5, 1e-12 );
        CHECK_NEAR( t, ms_between( at_us( 3500 ), at_us( 1000 ) ), -2.5, 1e-12 );
        CHECK_EQ( t, ms_between( at_us( 7 ), at_us( 7 ) ), 0.0 );
        // ms_since reads the clock after t0: never negative on a steady clock.
        const PerfClock::time_point t0 = PerfClock::now();
        CHECK( t, ms_since( t0 ) >= 0.0 );
    }

    void max_ms( TestRun& t )
    {
        MaxMs m;
        CHECK_EQ( t, m.take(), 0.0 );  // an empty window
        m.note( 3.0 );
        m.note( 1.0 );
        m.note( 7.5 );
        m.note( std::numeric_limits<double>::quiet_NaN() );  // never replaces the max
        m.note( -2.0 );
        CHECK_EQ( t, m.take(), 7.5 );
        CHECK_EQ( t, m.take(), 0.0 );  // take() starts the next window at 0
        m.note( -1.0 );                // below the 0 start: not kept
        CHECK_EQ( t, m.take(), 0.0 );
        m.note( 0.25 );
        CHECK_EQ( t, m.take(), 0.25 );
    }

    void counter_delta( TestRun& t )
    {
        CounterDelta d;
        CHECK_EQ( t, d.take( 5 ), 5U );  // the first take counts from 0
        CHECK_EQ( t, d.take( 5 ), 0U );
        CHECK_EQ( t, d.take( 12 ), 7U );
        // A delta above uint32 is truncated, as the static_cast at the old sites did.
        CHECK_EQ( t, d.take( 12 + ( uint64_t { 1 } << 32 ) + 3 ), 3U );
    }

    void lap( TestRun& t )
    {
        Lap l;
        double out = -1.0;
        CHECK( t, !l.lap( at_us( 1000 ), out ) );  // the first event: no interval
        CHECK_EQ( t, out, -1.0 );                  // untouched
        CHECK( t, l.lap( at_us( 1500 ), out ) );
        CHECK_NEAR( t, out, 0.5, 1e-12 );
        CHECK( t, l.lap( at_us( 4500 ), out ) );
        CHECK_NEAR( t, out, 3.0, 1e-12 );  // from the previous lap, not the first
        l.reset();
        out = -1.0;
        CHECK( t, !l.lap( at_us( 9000 ), out ) );  // after reset: the first event again
        CHECK_EQ( t, out, -1.0 );
        CHECK( t, l.lap( at_us( 9250 ), out ) );
        CHECK_NEAR( t, out, 0.25, 1e-12 );
    }
}  // namespace

void test_perf_clock( TestRun& t )
{
    elapsed( t );
    max_ms( t );
    counter_delta( t );
    lap( t );
}
