// light_ai_tests_tsan: the threaded suites only, built with -fsanitize=thread
// (tests/CMakeLists.txt). A data race makes TSan exit non-zero.

#include <iostream>

#include "check.hpp"
#include "suites.hpp"

int main()
{
    TestRun t;
    test_source_threads( t );
    std::cout << "light_ai_tests_tsan: " << t.checks() << " checks, " << t.failures() << " failed\n";
    return t.failures() == 0 ? 0 : 1;
}
