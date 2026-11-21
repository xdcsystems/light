#include <algorithm>
#include <array>
#include <cstddef>

#include "core/encode_pacer.hpp"

namespace
{
    constexpr int64_t kDefaultVfrStepUs = 33333;  // ~30 fps before sources report cadence
    constexpr int64_t kMinVfrStepUs = 33333;      // cap multi-view VFR at ~30 fps
    constexpr int64_t kMaxVfrStepUs = 200000;     // floor at 5 fps even for very slow cams
    constexpr int64_t kHeavyVfrStepUs = 40000;    // 25 fps cap under heavy packet backlog
    constexpr int64_t kMinSampleUs = 10000;       // ignore <10 ms bursts (e.g. duplicate polls)
    constexpr int64_t kMaxSampleUs = 500000;      // ignore >500 ms pauses/reconnects
    constexpr int64_t kStaleSourceUs = 1000000;   // silent >1 s: source detached, do not hold pace
    constexpr int kMinSamples = 3;                // require 3 intervals before trusting median

    int64_t median_period_us( const std::array<int64_t, EncodePacer::kCadenceWindow>& dts, int count ) noexcept
    {
        std::array<int64_t, EncodePacer::kCadenceWindow> tmp = dts;
        const auto mid = static_cast<std::ptrdiff_t>( count / 2 );
        std::nth_element( tmp.begin(), tmp.begin() + mid, tmp.begin() + static_cast<std::ptrdiff_t>( count ) );
        return tmp[static_cast<size_t>( mid )];
    }
}  // namespace

void EncodePacer::observe_source( int src_idx, int64_t ts_us ) noexcept
{
    if ( src_idx < 0 || src_idx >= kMaxSources || ts_us < 0 )
        return;
    SourceCadence& sc = cadence_[static_cast<size_t>( src_idx )];
    if ( sc.last_us >= 0 )
    {
        const int64_t dt = ts_us - sc.last_us;
        if ( dt > kStaleSourceUs )
        {
            sc.count = 0;
            sc.cursor = 0;
        }
        if ( dt >= kMinSampleUs && dt <= kMaxSampleUs )
        {
            sc.dts[static_cast<size_t>( sc.cursor )] = dt;
            sc.cursor = ( sc.cursor + 1 ) % kCadenceWindow;
            sc.count = std::min( sc.count + 1, static_cast<int>( kCadenceWindow ) );
        }
    }
    if ( sc.last_us < 0 || ts_us > sc.last_us )
        sc.last_us = ts_us;
}

int64_t EncodePacer::slowest_period_us( int64_t now_us ) const noexcept
{
    const int n = std::min( nsrc_, static_cast<int>( kMaxSources ) );
    int64_t slowest = 0;
    for ( int s = 0; s < n; ++s )
    {
        const SourceCadence& sc = cadence_[static_cast<size_t>( s )];
        if ( sc.last_us < 0 || sc.count < kMinSamples )
            continue;
        if ( now_us >= 0 && ( now_us - sc.last_us ) > kStaleSourceUs )
            continue;
        slowest = std::max( slowest, median_period_us( sc.dts, sc.count ) );
    }
    if ( slowest <= 0 )
        return kDefaultVfrStepUs;
    return std::clamp( slowest, kMinVfrStepUs, kMaxVfrStepUs );
}

void EncodePacer::enter( uint32_t epoch ) noexcept
{
    if ( epoch != epoch_ )
    {
        epoch_ = epoch;
        last_ts_us_ = -1;
    }
}

void EncodePacer::advance_grid( int64_t ts_us, int64_t step_us ) noexcept
{
    if ( ts_us < 0 || last_ts_us_ < 0 || step_us <= 0 )
    {
        last_ts_us_ = ts_us;
        return;
    }
    last_ts_us_ += step_us;
    if ( ( ts_us - last_ts_us_ ) >= step_us )
        last_ts_us_ = ts_us;
}

bool EncodePacer::admit( int64_t ts_us, int queue_depth, uint32_t epoch ) noexcept
{
    enter( epoch );
    if ( nsrc_ <= 1 || ts_us < 0 || last_ts_us_ < 0 )
    {
        last_ts_us_ = ts_us;
        return true;
    }
    if ( ts_us < last_ts_us_ )
        return false;
    int64_t min_step_us = 0;
    if ( fps_ > 0 )
    {
        min_step_us = 1000000 / fps_;
    }
    else
    {
        const int64_t base_vfr_us = slowest_period_us( ts_us );
        min_step_us = queue_depth >= kHeavyDepth ? std::max( base_vfr_us, kHeavyVfrStepUs ) : base_vfr_us;
    }
    const bool enc = ( ts_us - last_ts_us_ ) >= min_step_us;
    if ( enc )
        advance_grid( ts_us, min_step_us );
    return enc;
}

bool EncodePacer::admit_idle( int64_t ts_us, int queue_depth, uint32_t epoch ) noexcept
{
    enter( epoch );
    if ( queue_depth >= kBusyDepth )
        return false;
    if ( ts_us >= 0 && ( last_ts_us_ < 0 || ts_us > last_ts_us_ ) )
        last_ts_us_ = ts_us;
    return true;
}
