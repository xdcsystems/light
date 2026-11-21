// tidy_probe/tests/harness.cpp: rules-pass self-test fixture (#93): a file directly in a tests/ directory is the
// test harness, which prints with std::cout and printf, so custom-no-printf and custom-no-std-stream stay silent;
// the other rules still hold there (one function-local static: one hit in expected_rules.txt).
#include <cstdio>
#include <iostream>

int harness_count()
{
    static int calls = 0;  // custom-no-static-local
    std::printf( "calls %d\n", ++calls );
    std::cout << calls << '\n';
    return calls;
}
