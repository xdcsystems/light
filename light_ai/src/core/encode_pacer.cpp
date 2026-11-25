#include <algorithm>
#include <array>
#include <cstddef>

#include "core/encode_pacer.hpp"

namespace
{
    constexpr int64_t kDefaultVfrStepUs = 33333;   // ~30 fps before sources report cadence
    constexpr int64_t kMinVfrStepUs = 33333;       // cap multi-view VFR at ~30 fps
    constexpr int64_t kMaxVfrStepUs = 200000;      // floor at 5 fps even for very slow cams
    constexpr int64_t kHeavyVfrStepUs = 40000;     // 25 fps cap under heavy packet backlog
    constexpr int64_t kMinSampleUs = 10000;        // ignore <10 ms bursts (e.g. duplicate polls)
    constexpr int64_t kMaxSampleUs = 500000;       // ignore >500 ms pauses/reconnects
    constexpr int64_t kStaleSourceUs = 1000000;    // silent >1 s: source detached, do not hold pace
    constexpr int64_t kStepDownHoldUs = 200000;       // 200 ms sustained backlog before step-down
    constexpr int64_t kStepDownCooldownUs = 500000;   // min 500 ms between queue-driven step-downs
    constexpr int64_t kRecoveryHoldUs = 8000000;   // 8 s clean channel before stepping back up
    constexpr int kMinSamples = 3;                 // require 3 intervals before trusting median
    constexpr std::array<int64_t, 4> kBpFloorStepUs { 0, 50000, 80000, 150000 };

    int64_t median_period_us( const std::array<int64_t, EncodePacer::kCadenceWindow>& dts, int count ) noexcept
    {
        std::array<int64_t, EncodePacer::kCadenceWindow> tmp = dts;
        const auto mid = static_cast<std::ptrdiff_t>( count / 2 );
        std::nth_element( tmp.begin(), tmp.begin() + mid, tmp.begin() + static_cast<std::ptrdiff_t>( count ) );
        return tmp[static_cast<size_t>( mid )];
    }

    bool is_critical_backlog( int queue_depth, int64_t head_age_us ) noexcept
    {
        return queue_depth >= EncodePacer::kPauseDepth || head_age_us >= EncodePacer::kPauseAgeUs;
    }

    bool can_resume_backlog( int queue_depth, int64_t head_age_us ) noexcept
    {
        return queue_depth <= EncodePacer::kHeavyDepth && head_age_us <= EncodePacer::kHeavyAgeUs;
    }

    bool is_clean_backlog( int queue_depth, int64_t head_age_us ) noexcept
    {
        return queue_depth <= EncodePacer::kLowDepth && head_age_us <= EncodePacer::kLowAgeUs;
    }

    int target_level_for_backlog( int queue_depth, int64_t head_age_us ) noexcept
    {
        if ( is_critical_backlog( queue_depth, head_age_us ) )
            return 3;
        if ( queue_depth >= EncodePacer::kHeavyDepth || head_age_us >= EncodePacer::kHeavyAgeUs )
            return 2;
        if ( queue_depth >= EncodePacer::kBusyDepth || head_age_us >= EncodePacer::kBusyAgeUs )
            return 1;
        return 0;
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

void EncodePacer::observe_flush( int64_t ts_us, uint32_t epoch ) noexcept
{
    enter( epoch );
    bp_level_ = std::min( bp_level_ + 1, 3 );
    bp_low_since_us_ = -1;
    if ( ts_us >= 0 && ( bp_last_down_us_ < 0 || ts_us > bp_last_down_us_ ) )
        bp_last_down_us_ = ts_us;
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
        bp_level_ = 0;
        bp_paused_ = false;
        bp_high_since_us_ = -1;
        bp_low_since_us_ = -1;
        bp_last_down_us_ = -1;
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

void EncodePacer::update_backpressure( int64_t ts_us, int queue_depth, int64_t head_age_us ) noexcept
{
    if ( is_critical_backlog( queue_depth, head_age_us ) )
    {
        bp_paused_ = true;
        bp_level_ = 3;
        bp_high_since_us_ = -1;
        bp_low_since_us_ = -1;
    }
    if ( bp_paused_ && can_resume_backlog( queue_depth, head_age_us ) )
        bp_paused_ = false;
    if ( ts_us < 0 || ( last_ts_us_ >= 0 && ts_us < last_ts_us_ ) )
        return;

    const int target = target_level_for_backlog( queue_depth, head_age_us );
    if ( target >= 3 )
    {
        bp_last_down_us_ = ts_us;
        return;
    }
    if ( target > bp_level_ )
    {
        bp_low_since_us_ = -1;
        if ( bp_high_since_us_ < 0 )
        {
            bp_high_since_us_ = ts_us;
            return;
        }
        const bool held = ( ts_us - bp_high_since_us_ ) >= kStepDownHoldUs;
        const bool cooldown_ok = ( bp_last_down_us_ < 0 ) || ( ( ts_us - bp_last_down_us_ ) >= kStepDownCooldownUs );
        if ( held && cooldown_ok )
        {
            bp_level_ = std::min( bp_level_ + 1, target );
            bp_high_since_us_ = ts_us;
            bp_last_down_us_ = ts_us;
        }
        return;
    }
    bp_high_since_us_ = -1;
    if ( !is_clean_backlog( queue_depth, head_age_us ) )
    {
        bp_low_since_us_ = -1;
        return;
    }
    if ( bp_level_ == 0 )
        return;
    if ( bp_low_since_us_ < 0 )
    {
        bp_low_since_us_ = ts_us;
        return;
    }
    if ( ( ts_us - bp_low_since_us_ ) >= kRecoveryHoldUs )
    {
        --bp_level_;
        bp_low_since_us_ = ts_us;
    }
}

int64_t EncodePacer::effective_step_us( int64_t base_step_us, int queue_depth, int64_t head_age_us ) const noexcept
{
    int64_t step = base_step_us;
    if ( nsrc_ > 1 && fps_ <= 0 && ( queue_depth >= kHeavyDepth || head_age_us >= kHeavyAgeUs ) )
        step = std::max( step, kHeavyVfrStepUs );
    const int64_t bp_floor = kBpFloorStepUs[static_cast<size_t>( bp_level_ )];
    return std::max( step, bp_floor );
}

bool EncodePacer::admit( int64_t ts_us, int queue_depth, uint32_t epoch, int64_t head_age_us ) noexcept
{
    enter( epoch );
    update_backpressure( ts_us, queue_depth, head_age_us );
    if ( bp_paused_ )
        return false;
    if ( ( nsrc_ <= 1 && fps_ <= 0 && bp_level_ == 0 ) || ts_us < 0 || last_ts_us_ < 0 )
    {
        last_ts_us_ = ts_us;
        return true;
    }
    if ( ts_us < last_ts_us_ )
        return false;
    const int64_t base_step_us = ( fps_ > 0 ) ? ( 1000000 / fps_ ) : ( ( nsrc_ > 1 ) ? slowest_period_us( ts_us ) : 0 );
    const int64_t min_step_us = effective_step_us( base_step_us, queue_depth, head_age_us );
    const bool enc = ( ts_us - last_ts_us_ ) >= min_step_us;
    if ( enc )
        advance_grid( ts_us, min_step_us );
    return enc;
}

bool EncodePacer::admit_idle( int64_t ts_us, int queue_depth, uint32_t epoch, int64_t head_age_us ) noexcept
{
    enter( epoch );
    update_backpressure( ts_us, queue_depth, head_age_us );
    if ( bp_paused_ || queue_depth >= kBusyDepth || head_age_us >= kBusyAgeUs )
        return false;
    if ( ts_us >= 0 && ( last_ts_us_ < 0 || ts_us > last_ts_us_ ) )
        last_ts_us_ = ts_us;
    return true;
}
