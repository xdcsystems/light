// tidy_probe/violations.cpp: the self-test fixture of the clang-tidy rules pass (scripts/tidy_rules.yaml, #93).
// One violation of each rule; scripts/tidy_run.sh runs the rules pass on it before the gate and fails unless the
// hits per check are exactly expected_rules.txt (a matcher with a typo would match nothing). Never built, never
// in the TU set (tidy_run.sh takes only tests/*.cpp), skipped by scripts/rules_lint.sh.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

int g_counter = 0;                         // custom-no-global-var
static int file_static_fn()                // custom-no-file-static
{
    return 2;
}
thread_local const int t_local = 3;        // custom-no-thread-local (const: not a mutable global)

struct Base
{
    int b = 0;
};
struct Derived : Base                      // custom-no-inheritance
{
};
struct Iface
{
    virtual int f() const { return 1; }    // custom-no-virtual
};
struct WithStatic
{
    static int sfn() { return 4; }         // custom-no-static-member
};
struct Holder
{
    std::unique_ptr<int> p;                // custom-no-smart-ptr x2 (the field's type, the written type)
};
std::vector<std::shared_ptr<int>>* g_list = nullptr;  // custom-no-global-var, custom-no-smart-ptr (a template argument)

// readability-function-cognitive-complexity: 1 + 2 + 3 + 4 + 5 + (6 + 1) = 22 > 20
int tangled( int a, int b, int c )
{
    int r = 0;
    for ( int i = 0; i < a; ++i )
        for ( int j = 0; j < b; ++j )
            for ( int k = 0; k < c; ++k )
                if ( i == j )
                    if ( j == k )
                        if ( i > 0 && k > 0 )
                            ++r;
    return r;
}

int use()
{
    static int local = 0;                  // custom-no-static-local
    int* p = new int( 1 );                 // custom-no-new-delete
    const int v = *p;
    delete p;                              // custom-no-new-delete
    void* m = std::malloc( 4 );            // custom-no-new-delete (the malloc family)
    std::free( m );                        // custom-no-new-delete
    void* raw = ::operator new( 8 );       // custom-no-new-delete (operator new)
    ::operator delete( raw );              // custom-no-new-delete (operator delete)
    std::make_shared<int>( 6 );            // custom-no-smart-ptr (a discarded make_shared)
    const int k = 5;
    const int* q = const_cast<int*>( &k ); // custom-no-const-cast
    std::printf( "%d\n", v );              // custom-no-printf
    std::fwrite( "x", 1, 1, stdout );      // custom-no-printf (C stdio output)
    std::cout << local;                    // custom-no-std-stream
    std::wcout << L"w";                    // custom-no-std-stream (the w- form)
    try
    {
        std::cout.flush();                 // custom-no-std-stream
    }
    catch ( ... )                          // custom-no-catch-all
    {
    }
    return *q + g_counter + static_cast<int>( g_list != nullptr ) + file_static_fn() + t_local + Iface {}.f() + WithStatic::sfn() + Derived {}.b
           + static_cast<int>( Holder {}.p != nullptr ) + tangled( 1, 1, 1 );
}
