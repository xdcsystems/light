// TrailWindow: the spawn point, the full-window gate, the median, the step
// gate, eviction, recent() order, the counter fold.

#include <array>

#include "core/trail_window.hpp"
#include "check.hpp"
#include "suites.hpp"

void test_trail_window( TestRun& t )
{
    TrailWindow w;
    w.restart( 0, 0.0f, 0.0f );
    CHECK( t, w.size() == 1 );
    std::array<TrajPoint, 8> pts {};
    CHECK( t, w.recent( 1, pts.data(), 8 ) == 1 && pts[0].ts_us == 0 && pts[0].x == 0.0f );

    // The window counts from zero after restart: four posteriors push nothing,
    // the fifth pushes the median (100, 0) -- 100 px from the spawn point.
    for ( int i = 1; i <= 4; ++i )
        w.feed( i, 100.0f, 0.0f );
    CHECK( t, w.size() == 1 );
    w.feed( 5, 100.0f, 0.0f );
    CHECK( t, w.size() == 2 );

    // One outlier does not move the median: no push. Alternation A, B, A, B
    // around 100 keeps the median at 100: no push either.
    w.feed( 6, 160.0f, 0.0f );
    CHECK( t, w.size() == 2 );
    w.feed( 7, 100.0f, 0.0f );
    w.feed( 8, 160.0f, 0.0f );
    CHECK( t, w.size() == 2 );
    // A real move: the median follows once three of five are there.
    w.feed( 9, 160.0f, 0.0f );
    CHECK( t, w.size() == 3 );
    CHECK( t, w.recent( 1, pts.data(), 8 ) == 3 && pts[2].ts_us == 9 && pts[2].x == 160.0f );

    // Below the 8 px step: no push; at 8 px: a push.
    for ( int i = 10; i <= 14; ++i )
        w.feed( i, 167.0f, 0.0f );
    CHECK( t, w.size() == 3 );
    for ( int i = 15; i <= 17; ++i )
        w.feed( i, 168.0f, 0.0f );
    CHECK( t, w.size() == 4 );

    // recent(): oldest first, newest always included, every stride-th from the newest.
    CHECK( t, w.recent( 2, pts.data(), 8 ) == 2 && pts[0].ts_us == 5 && pts[1].ts_us == 17 );
    CHECK( t, w.recent( 1, pts.data(), 2 ) == 2 && pts[0].ts_us == 9 && pts[1].ts_us == 17 );

    // Eviction from the head.
    w.evict( 6 );
    CHECK( t, w.size() == 2 );
    w.evict( 100 );
    CHECK( t, w.empty() );

    // A long walk from a fresh spawn: the window counter folds back (no
    // overflow), every step pushes (the median moves 10 px per feed) until
    // the ring is full -- then PodRing drops the new points: the spawn point
    // plus the medians of feeds 5 .. 303 (the median of feed i is 10 * (i - 2)).
    TrailWindow walk;
    walk.restart( 0, 0.0f, 0.0f );
    for ( int i = 1; i <= 1000; ++i )
        walk.feed( i, 10.0f * static_cast<float>( i ), 0.0f );
    CHECK( t, walk.size() == TrailWindow::kTrajPoints );
    CHECK( t, walk.recent( 1, pts.data(), 1 ) == 1 && pts[0].ts_us == 303 && pts[0].x == 3010.0f );
}
