#include "check.hpp"
#include "core/session_warmup.hpp"
#include "suites.hpp"

namespace
{
    void all_cells( TestRun& t )
    {
        // Two cells: held until both have a picture of this session.
        SessionWarmup w( 2, 200000 );
        CHECK( t, w.enter( 1, 1000 ) );
        CHECK( t, !w.ready( 1000 ) );
        w.refreshed( 0 );
        CHECK( t, !w.ready( 2000 ) );
        w.refreshed( 0 );  // the same cell again does not release
        CHECK( t, !w.ready( 3000 ) );
        w.refreshed( 1 );
        CHECK( t, w.ready( 4000 ) );
        // The same epoch again changes nothing; the session stays released.
        CHECK( t, !w.enter( 1, 5000 ) );
        CHECK( t, w.ready( 5000 ) );
    }

    void timeout( TestRun& t )
    {
        // A live source that delivers nothing: released when the hold expires,
        // its cell stays as reset (gray).
        SessionWarmup w( 3, 200000 );
        CHECK( t, w.enter( 1, 1000000 ) );
        w.refreshed( 0 );
        w.refreshed( 2 );
        CHECK( t, !w.ready( 1199999 ) );
        CHECK( t, w.ready( 1200000 ) );
        // Latched: an earlier time does not hold the session again.
        CHECK( t, w.ready( 1000000 ) );
        // Nothing refreshed at all: the clock alone releases.
        SessionWarmup e( 2, 200000 );
        CHECK( t, e.enter( 7, 0 ) );
        CHECK( t, !e.ready( 199999 ) );
        CHECK( t, e.ready( 200000 ) );
    }

    void epochs( TestRun& t )
    {
        // A new epoch forgets the cells and restarts the clock.
        SessionWarmup w( 2, 200000 );
        CHECK( t, w.enter( 1, 0 ) );
        w.refreshed( 0 );
        w.refreshed( 1 );
        CHECK( t, w.ready( 0 ) );
        CHECK( t, w.enter( 2, 500000 ) );
        CHECK( t, !w.ready( 500000 ) );
        w.refreshed( 1 );
        CHECK( t, !w.ready( 600000 ) );
        CHECK( t, w.ready( 700000 ) );  // 200 ms after the new epoch's start
        // Refreshes of the previous epoch do not count.
        w.refreshed( 0 );
        CHECK( t, w.enter( 3, 800000 ) );
        w.refreshed( 1 );
        CHECK( t, !w.ready( 800001 ) );
        // Epoch 0 is a valid first epoch (no session entered yet).
        SessionWarmup z( 1, 200000 );
        CHECK( t, z.enter( 0, 0 ) );
        CHECK( t, !z.enter( 0, 1 ) );
    }

    void single_and_range( TestRun& t )
    {
        // One cell (single view): released by its first refresh.
        SessionWarmup s( 1, 200000 );
        CHECK( t, s.enter( 1, 0 ) );
        CHECK( t, !s.ready( 0 ) );
        s.refreshed( 0 );
        CHECK( t, s.ready( 0 ) );
        // Cells outside 0..ncells-1 are ignored: they neither release nor break the mask.
        SessionWarmup w( 2, 200000 );
        CHECK( t, w.enter( 1, 0 ) );
        w.refreshed( -1 );
        w.refreshed( 2 );
        w.refreshed( 31 );
        w.refreshed( 99 );
        CHECK( t, !w.ready( 1 ) );
        w.refreshed( 0 );
        w.refreshed( 1 );
        CHECK( t, w.ready( 2 ) );
        // ncells is clamped: 0 waits for one cell.
        SessionWarmup c( 0, 200000 );
        CHECK( t, c.enter( 1, 0 ) );
        c.refreshed( 0 );
        CHECK( t, c.ready( 0 ) );
    }
}  // namespace

void test_session_warmup( TestRun& t )
{
    all_cells( t );
    timeout( t );
    epochs( t );
    single_and_range( t );
}
