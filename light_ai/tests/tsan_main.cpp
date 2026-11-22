// light_ai_tests_tsan: the 11 threaded suites only (sibling unthreaded suites are linked in
// but not run here), built with -fsanitize=thread (tests/CMakeLists.txt). A data race makes
// TSan exit non-zero. Arguments (a suite filter, --repeat N): tests/suite_runner.hpp,
// e.g. light_ai_tests_tsan --repeat 200.

#include <iostream>

#include "check.hpp"
#include "suite_runner.hpp"
#include "suites.hpp"

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the type of main's argv as char**
int main( int argc, char** argv )
{
    const Suite suites[] = {
        { "source_threads", &test_source_threads },
        { "source_prober_threads", &test_source_prober_threads },
        { "preview_ring_threads", &test_preview_ring_threads },
        { "packet_queue_threads", &test_packet_queue_threads },
        { "session_gate_threads", &test_session_gate_threads },
        { "reid_worker_tsan", &test_reid_worker_tsan },
        { "pi_mutex", &test_pi_mutex },
        { "async_log_sink_threads", &test_async_log_sink_threads },
        { "infer_slot_threads", &test_infer_slot_threads },
        { "stream_output_threads", &test_stream_output_threads },
        { "tcp_pull_threads", &test_tcp_pull_threads },
    };
    return run_suites( "light_ai_tests_tsan", suites, argc, argv, std::cout, std::cerr );
}
