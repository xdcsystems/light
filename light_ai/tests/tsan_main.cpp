// light_ai_tests_tsan: the 5 threaded suites only (source_threads, source_prober_threads, packet_queue_threads,
// session_gate_threads, reid_worker_tsan; sibling unthreaded suites are linked in but not run here), built with -fsanitize=thread
// (tests/CMakeLists.txt). A data race makes TSan exit non-zero. Arguments (a suite filter, --repeat N):
// tests/suite_runner.hpp, e.g. light_ai_tests_tsan --repeat 200.

#include <iostream>

#include "check.hpp"
#include "suite_runner.hpp"
#include "suites.hpp"

int main( int argc, char** argv )
{
    const Suite suites[] = {
        { "source_threads", &test_source_threads },
        { "source_prober_threads", &test_source_prober_threads },
        { "packet_queue_threads", &test_packet_queue_threads },
        { "session_gate_threads", &test_session_gate_threads },
        { "reid_worker_tsan", &test_reid_worker_tsan },
    };
    return run_suites( "light_ai_tests_tsan", suites, argc, argv, std::cout, std::cerr );
}
