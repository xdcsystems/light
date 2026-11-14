// light_ai_tests_tsan: the threaded suites only (test_source_prober.cpp, test_packet_queue.cpp and test_session_gate.cpp also
// hold their unthreaded suites, linked in but not run here), built with -fsanitize=thread
// (tests/CMakeLists.txt). A data race makes TSan exit non-zero.

#include <iostream>

#include "check.hpp"
#include "suites.hpp"

int main()
{
    TestRun t;
    test_source_threads( t );
    test_source_prober_threads( t );
    test_packet_queue_threads( t );
    test_session_gate_threads( t );
    std::cout << "light_ai_tests_tsan: " << t.checks() << " checks, " << t.failures() << " failed\n";
    return t.failures() == 0 ? 0 : 1;
}
