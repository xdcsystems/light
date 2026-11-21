#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/fixed_fmt.hpp"
#include "core/log.hpp"
#include "platform/log_sink.hpp"
#include "platform/pi_mutex.hpp"
#include "suites.hpp"

void test_pi_mutex( TestRun& t )
{
    // 1. PiMutex + PiCondVar producer/consumer handoff (TSan-verified).
    {
        light_ai::platform::PiMutex mx;
        CHECK( t, mx.prio_inherit_enabled() );
        light_ai::platform::PiCondVar cv;
        int value = 0;
        bool ready = false;
        bool stop = false;
        int sum = 0;

        std::thread consumer( [&]() {
            for ( bool done = false; !done; )
            {
                std::unique_lock<light_ai::platform::PiMutex> lk( mx );
                cv.wait( lk, [&]() { return ready || stop; } );
                if ( !ready && stop )
                {
                    done = true;
                    continue;
                }
                sum += value;
                ready = false;
                cv.notify_one();
            }
        } );

        for ( int i = 1; i <= 100; ++i )
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx );
            cv.wait( lk, [&]() { return !ready; } );
            value = i;
            ready = true;
            cv.notify_one();
        }
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx );
            cv.wait( lk, [&]() { return !ready; } );
            stop = true;
            cv.notify_all();
        }
        consumer.join();
        CHECK_EQ( t, sum, 5050 );

        // wait_for timeout check.
        std::unique_lock<light_ai::platform::PiMutex> lk( mx );
        const bool pred = cv.wait_for( lk, std::chrono::milliseconds( 2 ), []() { return false; } );
        CHECK( t, !pred );
    }

    // 2. AsyncLogSink multi-producer lock-free logging + Fixed/hex formatting.
    {
        std::ostringstream out;
        std::ostringstream err;
        {
            light_ai::platform::AsyncLogSink sink( out, err, false );
            const Log log( sink.sink_fn(), Log::Level::Info );
            log.info() << "fixed=" << Fixed { 12.34, 1 } << " hex=0x" << std::hex << 255 << std::dec << '\n';
            log.warn() << "warn_line\n";

            constexpr int k_threads = 4;
            constexpr int k_lines = 50;
            std::vector<std::thread> pool;
            pool.reserve( k_threads );
            for ( int id = 0; id < k_threads; ++id )
            {
                pool.emplace_back( [&log, id]() {
                    for ( int n = 0; n < k_lines; ++n )
                        log.info() << "async " << id << ' ' << n << '\n';
                } );
            }
            for ( std::thread& th : pool )
                th.join();
        }
        CHECK( t, out.str().find( "fixed=12.3 hex=0xff\n" ) != std::string::npos );
        CHECK( t, err.str().find( "warn_line\n" ) != std::string::npos );
    }
}
