#include "core/pts_clock.hpp"

// us * 90000 / 1000000 = us * 9 / 100; adding half the divisor rounds to
// nearest, and the negative branch mirrors it (halves away from zero).
int64_t us_to_90k( int64_t us )
{
    return us >= 0 ? ( us * 9 + 50 ) / 100 : -( ( -us * 9 + 50 ) / 100 );
}

void PtsClock::restart( int64_t frame_step )
{
    counter_ = 0;
    base_us_ = -1;
    last_pts_ = -1;
    frame_step_ = frame_step < 1 ? 1 : frame_step;
}

int64_t PtsClock::next( int64_t ts_us, uint64_t frames_sent )
{
    if ( !vfr_ )
        return static_cast<int64_t>( counter_ );
    if ( ts_us < 0 )
        return static_cast<int64_t>( frames_sent ) * 3600;  // 25 fps grid
    if ( base_us_ < 0 )
        base_us_ = ts_us;  // per-session base on the first timestamp
    int64_t p = us_to_90k( ts_us - base_us_ );
    if ( last_pts_ >= 0 && p <= last_pts_ )
        p = last_pts_ + 1;
    last_pts_ = p;
    return p;
}
