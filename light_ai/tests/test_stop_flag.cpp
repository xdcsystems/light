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
        std::atomic<bool> quit { false };
        const auto t0 = std::chrono::steady_clock::now();
        const bool interrupted = sleep_interruptible( quit, 300 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        CHECK( t, !interrupted );
        CHECK( t, el >= 300 );
    }
    // Pre-set quit: no sleep at all, returns at once (true).
    {
        std::atomic<bool> quit { true };
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
}
