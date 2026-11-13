#include "check.hpp"
#include "pod_list.hpp"
#include "pod_ring.hpp"
#include "suites.hpp"

void test_pod_containers( TestRun& t )
{
    // PodList: push fills up to the cap, then drops and reports false.
    PodList<int, 3> list;
    CHECK( t, list.n == 0 );
    CHECK( t, list.push( 10 ) && list.push( 20 ) && list.push( 30 ) );
    CHECK( t, !list.push( 40 ) );
    CHECK( t, list.n == 3 && list.d[0] == 10 && list.d[2] == 30 );
    list.clear();
    CHECK( t, list.n == 0 );

    // PodRing: FIFO order, overflow drop, wrap-around, back(k) from the newest.
    PodRing<int, 3> ring;
    CHECK( t, ring.empty() && !ring.full() && ring.size() == 0 );
    CHECK( t, ring.push( 1 ) && ring.push( 2 ) && ring.push( 3 ) );
    CHECK( t, ring.full() && !ring.push( 4 ) );
    CHECK( t, ring.front() == 1 && ring.back( 0 ) == 3 && ring.back( 2 ) == 1 );
    ring.pop();
    CHECK( t, ring.push( 4 ) );  // wraps into the freed slot
    CHECK( t, ring.size() == 3 && ring.front() == 2 && ring.back( 0 ) == 4 && ring.back( 1 ) == 3 );
    ring.clear();
    CHECK( t, ring.empty() && ring.size() == 0 );
    ring.push_slot() = 7;  // in-place fill: the caller guarantees !full()
    CHECK( t, ring.size() == 1 && ring.front() == 7 );
}
