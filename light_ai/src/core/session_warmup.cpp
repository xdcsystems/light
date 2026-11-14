#include <algorithm>

#include "core/session_warmup.hpp"

SessionWarmup::SessionWarmup( int ncells, int64_t hold_us )
  : full_mask_( ( 1U << std::clamp( ncells, 1, static_cast<int>( kMaxCells ) ) ) - 1U )
  , hold_us_( hold_us )
{
}

bool SessionWarmup::enter( uint32_t epoch, int64_t now_us )
{
    if ( started_ && epoch == epoch_ )
        return false;
    started_ = true;
    epoch_ = epoch;
    start_us_ = now_us;
    mask_ = 0;
    released_ = false;
    return true;
}

void SessionWarmup::refreshed( int cell )
{
    if ( cell >= 0 && cell < static_cast<int>( kMaxCells ) )
        mask_ |= ( 1U << cell ) & full_mask_;
}

bool SessionWarmup::ready( int64_t now_us )
{
    if ( !released_ )
        released_ = mask_ == full_mask_ || now_us - start_us_ >= hold_us_;
    return released_;
}
