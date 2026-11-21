// tidy_probe/src/main.cpp: rules-pass self-test fixture (#93): a path ending in src/main.cpp owns std::cout and
// std::cerr, so custom-no-std-stream stays silent here; expected_rules.txt lists no hit for this file.
#include <iostream>

int main()
{
    std::cout << "ok\n";
    std::cerr << "err\n";
    return 0;
}
