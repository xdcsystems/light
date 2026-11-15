#include <algorithm>

#include "core/trail_window.hpp"

namespace
{
    // Median of 5 values, 7-comparator selection network: the minimal
    // oblivious net (an exhaustive search confirms no 6-comparator median
    // network exists), loop-free, ~7 branches vs ~10-19 for the former
    // insertion sort. Reorders `v` in place; callers pass a disposable copy.
    float median_of_5( float* v )
    {
        const auto order2 = []( float& a, float& b ) {
            if ( b < a )
            {
                const float t = a;
                a = b;
                b = t;
            }
        };
        order2( v[0], v[1] );
        order2( v[2], v[3] );
        order2( v[0], v[2] );
        order2( v[1], v[3] );
        order2( v[1], v[2] );
        order2( v[1], v[4] );
        order2( v[2], v[4] );
        return v[2];
    }
}  // namespace

void TrailWindow::restart( int64_t ts_us, float x, float y )
{
    // Slot reuse: the median window belongs to the previous occupant. A stale
    // mwin_n would mix old posteriors into the first trail median -- the
    // aged-out reset in evict() cannot fire for a fresh spawn, the spawn
    // point is already in the ring (x/y match the KF posterior at spawn).
    mwin_n_ = 0;
    mwin_x_.fill( x );
    mwin_y_.fill( y );
    ring_.clear();
    ring_.push( TrajPoint { ts_us, x, y } );
}

void TrailWindow::feed( int64_t ts_us, float x, float y )
{
    static_assert( kTrailMedianN == 5, "median_of_5 is the median of the window" );
    // The window was reset by restart() or by the evict() that emptied the
    // ring; it is never reset here (#37: a reset on every feed while the ring
    // is empty kept the counter at 1 and the trail never came back).
    mwin_x_[mwin_n_ % kTrailMedianN] = x;
    mwin_y_[mwin_n_ % kTrailMedianN] = y;
    ++mwin_n_;
    // int overflow guard: past the first full window the counter only
    // feeds the modulo index above, so folding it back by one window
    // keeps both the index cycle and the full-window gate bit-exact.
    if ( mwin_n_ >= 2 * kTrailMedianN )
        mwin_n_ = kTrailMedianN;
    if ( mwin_n_ < kTrailMedianN )
        return;
    std::array<float, kTrailMedianN> xs = mwin_x_;
    std::array<float, kTrailMedianN> ys = mwin_y_;
    const float mx = median_of_5( xs.data() );
    const float my = median_of_5( ys.data() );
    if ( ring_.empty() || far_enough( mx, my ) )
        ring_.push( TrajPoint { ts_us, mx, my } );
}

// The step gate against the newest pushed point.
bool TrailWindow::far_enough( float mx, float my ) const
{
    const TrajPoint last = ring_.back( 0 );
    const float dx = mx - last.x;
    const float dy = my - last.y;
    const float step = static_cast<float>( kTrajMinStepPx );
    return dx * dx + dy * dy >= step * step;
}

void TrailWindow::evict( int64_t cutoff_us )
{
    const bool had_points = !ring_.empty();
    while ( !ring_.empty() && ring_.front().ts_us < cutoff_us )
        ring_.pop();
    // The trail aged out: reset the window once, at this transition. The next
    // kTrailMedianN feeds overwrite every slot before the full-window gate
    // opens, so the first post-gap median holds no pre-gap posterior; the ring
    // is empty, so it is pushed without the step gate (as the spawn point is).
    if ( had_points && ring_.empty() )
        mwin_n_ = 0;
}

// Oldest-first output, newest point always included: walk the ring backwards,
// keep every `stride`-th hit.
int TrailWindow::recent( int stride, TrajPoint* dst, int dst_cap ) const
{
    const int n = ring_.size();
    int w = 0;  // write index into dst
    for ( int k = 0; k < n && w < dst_cap; ++k )
    {
        if ( k % stride != 0 )
            continue;
        dst[w] = ring_.back( k );  // back(k): newest (k = 0) -> oldest
        ++w;
    }
    std::reverse( dst, dst + w );
    return w;
}
