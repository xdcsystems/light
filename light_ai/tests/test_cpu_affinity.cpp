// test_cpu_affinity.cpp: pin_current_thread_to_big_cores is noexcept and survives a throwing Log stream (#96).
// The pin runs inside its own std::thread, so the runner's CPU mask stays untouched.

#include <cstdint>
#include <fstream>
#include <ios>
#include <ostream>
#include <sched.h>
#include <sstream>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/log.hpp"
#include "sys/cpu_affinity.hpp"
#include "suites.hpp"

void test_cpu_affinity( TestRun& t )
{
    // A filebuf that is never opened: its overflow() returns EOF, so any write sets badbit. With
    // exceptions( badbit ) the stream then throws std::ios_base::failure out of the Log::Line.
    std::filebuf dead_out;
    std::filebuf dead_err;
    std::ostream out( &dead_out );
    std::ostream err( &dead_err );
    out.exceptions( std::ios::badbit );
    err.exceptions( std::ios::badbit );
    const Log log( out, err, Log::Level::Debug );

    static_assert( noexcept( pin_current_thread_to_big_cores( "x", log ) ), "the pin is called from noexcept thread bodies" );
    static_assert( ( kRtSchedPolicy & SCHED_RESET_ON_FORK ) != 0, "threads of an RT caller must start as SCHED_OTHER" );
    static_assert( ( kRtSchedPolicy & ~SCHED_RESET_ON_FORK ) == SCHED_FIFO, "the RT policy is SCHED_FIFO" );

    // The fixture really throws: a direct stream write with the same setup.
    {
        std::filebuf dead;
        std::ostream probe( &dead );
        probe.exceptions( std::ios::badbit );
        bool threw = false;
        try
        {
            probe << "pin: probe\n";
        }
        catch ( const std::ios_base::failure& )
        {
            threw = true;
        }
        CHECK( t, threw );
    }

    // Either log line (info on success, warn on a failed sched_setaffinity) throws; the pin swallows it.
    // A leaked throw would hit the noexcept of the pin and call std::terminate: reaching the next line is the check.
    bool returned = false;
    std::thread worker( [&log, &returned]() noexcept {
        pin_current_thread_to_big_cores( "test worker", log );
        returned = true;
    } );
    worker.join();
    CHECK( t, returned );
    CHECK( t, out.bad() != err.bad() );  // exactly one line: info to out (pinned) or warn to err (fallback)

    uint64_t minflt = 0;
    uint64_t majflt = 0;
    static_assert( noexcept( prefault_current_thread_stack() ), "prefault must be noexcept" );
    static_assert( noexcept( configure_rt_allocator( log ) ), "configure_rt_allocator must be noexcept" );
    static_assert( noexcept( read_thread_page_faults( minflt, majflt ) ), "read_thread_page_faults must be noexcept" );
    prefault_current_thread_stack( 64 * 1024 );
    read_thread_page_faults( minflt, majflt );

    // A priority above the SCHED_FIFO range is clamped, and the log names the clamped value (granted or refused).
    std::ostringstream rt_log_text;
    const Log rt_log( rt_log_text, rt_log_text );
    std::thread rt_worker( [&rt_log]() noexcept { pin_current_thread_rt( "rt clamp", -1, 1000, rt_log ); } );
    rt_worker.join();
    const std::string want = "rt: rt clamp -> SCHED_FIFO prio " + std::to_string( sched_get_priority_max( SCHED_FIFO ) );
    CHECK( t, rt_log_text.str().find( want ) != std::string::npos );
}
