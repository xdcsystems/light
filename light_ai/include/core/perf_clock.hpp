// Small timing helpers for the [perf] telemetry and the per-stage timings (#110):
//   ms_between / ms_since -- elapsed milliseconds between steady-clock stamps;
//   MaxMs        -- the largest sample of a window, read and cleared by take();
//   CounterDelta -- a monotonic counter as per-window deltas;
//   Lap          -- the time since the previous event, none before the first one.
// std only, no allocation. Not thread-safe: each instance belongs to one thread.

#pragma once

#include <chrono>
#include <cstdint>

using PerfClock = std::chrono::steady_clock;

// Milliseconds from a to b (negative when b is earlier).
inline double ms_between( PerfClock::time_point a, PerfClock::time_point b ) noexcept
{
    return std::chrono::duration<double, std::milli>( b - a ).count();
}

// Milliseconds from t0 to now.
inline double ms_since( PerfClock::time_point t0 ) noexcept
{
    return ms_between( t0, PerfClock::now() );
}

// The window maximum: note() keeps the larger sample (a NaN never replaces
// it), take() returns it and starts the next window at 0.
class MaxMs
{
public:
    void note( double ms ) noexcept
    {
        if ( ms > max_ )
            max_ = ms;
    }

    double take() noexcept
    {
        const double out = max_;
        max_ = 0.0;
        return out;
    }

private:
    double max_ = 0.0;
};

// A monotonic uint64 counter read once per window: take( now ) returns the
// growth since the previous take (the first one: since 0), truncated to
// uint32, and remembers now.
class CounterDelta
{
public:
    uint32_t take( uint64_t now ) noexcept
    {
        const auto out = static_cast<uint32_t>( now - prev_ );
        prev_ = now;
        return out;
    }

private:
    uint64_t prev_ = 0;
};

// The interval between consecutive events: lap( now, out_ms ) sets out_ms to
// the time since the previous lap and returns true; the first lap (and the
// first after reset()) only records now and returns false, out_ms untouched.
class Lap
{
public:
    bool lap( PerfClock::time_point now, double& out_ms ) noexcept
    {
        const bool had = has_last_;
        if ( had )
            out_ms = ms_between( last_, now );
        last_ = now;
        has_last_ = true;
        return had;
    }

    void reset() noexcept { has_last_ = false; }

private:
    PerfClock::time_point last_;
    bool has_last_ = false;
};
