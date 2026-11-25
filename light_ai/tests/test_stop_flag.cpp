#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/stop_flag.hpp"
#include "suites.hpp"

void test_stop_flag( TestRun& t )
{
    // Full elapsed: false, and the wait really waited.
    {
        const std::atomic<bool> quit { false };
        const auto t0 = std::chrono::steady_clock::now();
        const bool interrupted = sleep_interruptible( quit, 300 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        CHECK( t, !interrupted );
        CHECK( t, el >= 300 );
    }
    // Pre-set quit: no sleep at all, returns at once (true).
    {
        const std::atomic<bool> quit { true };
        const auto t0 = std::chrono::steady_clock::now();
        CHECK( t, sleep_interruptible( quit, 60 * 1000 ) );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        CHECK( t, el < 100 );
    }
    // Quit set from another thread mid-sleep: the wait ends early (the
    // wake slice is 100 ms), well under a second whatever the period.
    {
        std::atomic<bool> quit { false };
        std::thread wake( [&quit] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            quit.store( true );
        } );
        const bool interrupted = sleep_interruptible( quit, 60 * 1000 );
        wake.join();
        CHECK( t, interrupted );
    }
    // attach_wake(): callback fires on request() and wake_fd() reflects attached descriptor.
    {
        StopFlag sf;
        CHECK_EQ( t, sf.wake_fd(), -1 );
        struct Counter
        {
            mutable int calls = 0;
        };
        const Counter counter {};
        const auto cb = []( const void* ctx ) noexcept { ++static_cast<const Counter*>( ctx )->calls; };
        sf.attach_wake( 42, &counter, cb );
        CHECK_EQ( t, sf.wake_fd(), 42 );
        sf.request();
        CHECK( t, sf.requested() );
        CHECK_EQ( t, counter.calls, 1 );
        sf.attach_wake( -1, nullptr, nullptr );
        CHECK_EQ( t, sf.wake_fd(), -1 );
    }
}
