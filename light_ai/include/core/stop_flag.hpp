// StopFlag: a one-way "please stop" request shared between the owner of the
// shutdown decision (SignalWaiter, on SIGINT/SIGTERM) and the loops that
// honor it (Pipeline). Set once, never cleared. No SDK dependencies.
//
// Lifetime: declared in main() BEFORE the SignalWaiter and the Pipeline,
// so it outlives both -- neither side can ever see a dangling flag
// (this replaces the global g_pipe pointer and its lifetime guard).

#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>

class StopFlag
{
public:
    StopFlag() = default;
    StopFlag( const StopFlag& ) = delete;
    StopFlag& operator=( const StopFlag& ) = delete;
    StopFlag( StopFlag&& ) = delete;
    StopFlag& operator=( StopFlag&& ) = delete;
    ~StopFlag() = default;

    void request() { flag_.store( true, std::memory_order_release ); }
    bool requested() const { return flag_.load( std::memory_order_acquire ); }
    const std::atomic<bool>& raw() const noexcept { return flag_; }

private:
    std::atomic<bool> flag_ { false };
};

// A bounded wait interruptible by a stop flag: sleeps ms in 100 ms slices,
// checking quit between the slices. true = quit was requested (the caller
// exits at once), false = the full ms elapsed. A free function, not a
// StopFlag member: the serve threads' quit_ is a plain std::atomic<bool>,
// not the signal bridge; the prober keeps its cv (a wake at once), this
// trades up to one slice of wake latency for no mutex at all.
inline bool sleep_interruptible( const std::atomic<bool>& quit, int ms )
{
    constexpr int k_slice_ms = 100;  // the wake cadence: a local constexpr, not an enum (see README, Statics)
    int left = ms;
    while ( left > 0 && !quit.load( std::memory_order_acquire ) )
    {
        const int slice = std::min( left, k_slice_ms );
        std::this_thread::sleep_for( std::chrono::milliseconds( slice ) );
        left -= slice;
    }
    return quit.load( std::memory_order_acquire );
}
