// test_cpu_affinity.cpp: pin_current_thread_to_big_cores is noexcept and survives a throwing Log stream (#96).
// The pin runs inside its own std::thread, so the runner's CPU mask stays untouched.

#include <fstream>
#include <ios>
#include <ostream>
#include <thread>

#include "check.hpp"
#include "core/log.hpp"
#include "cpu_affinity.hpp"
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
}
