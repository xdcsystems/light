// StopFlag: a one-way "please stop" request shared between the owner of the
// shutdown decision (SignalWaiter, on SIGINT/SIGTERM) and the loops that
// honor it (Pipeline). Set once, never cleared. No SDK dependencies.
//
// Lifetime: declared in main() BEFORE the SignalWaiter and the Pipeline,
// so it outlives both -- neither side can ever see a dangling flag
// (this replaces the global g_pipe pointer and its lifetime guard).

#pragma once

#include <atomic>

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

private:
    std::atomic<bool> flag_ { false };
};
