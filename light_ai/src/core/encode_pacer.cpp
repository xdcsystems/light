#include "core/encode_pacer.hpp"

void EncodePacer::enter( uint32_t epoch )
{
    if ( epoch != epoch_ )
    {
        epoch_ = epoch;
        last_ts_us_ = -1;
    }
}

bool EncodePacer::admit( int64_t ts_us, int queue_depth, uint32_t epoch )
{
    enter( epoch );
    const int64_t vfr_step_us = queue_depth >= kBusyDepth ? 20000 : 4000;
    const int64_t min_step_us = nsrc_ > 1 ? ( fps_ > 0 ? 1000000 / fps_ : vfr_step_us ) : 0;
    const bool enc = nsrc_ <= 1 || ts_us < 0 || last_ts_us_ < 0 || ( ts_us - last_ts_us_ ) >= min_step_us;
    if ( enc )
        last_ts_us_ = ts_us;
    return enc;
}

bool EncodePacer::admit_idle( int64_t ts_us, int queue_depth, uint32_t epoch )
{
    enter( epoch );
    if ( queue_depth >= kBusyDepth )
        return false;
    last_ts_us_ = ts_us;
    return true;
}
