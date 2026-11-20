// tidy_probe/violations.cpp: the self-test fixture of the clang-tidy rules pass (scripts/tidy_rules.yaml, #93).
// Each rule's hits, and below them the near misses that must stay silent. scripts/tidy_run.sh runs the rules pass on
// it before the gate and fails unless the hit sites ("<fixture>:<line> <check>") are exactly expected_rules.txt: a
// matcher with a typo matches nothing, and one that drifts to another construct moves a hit off its line. The
// comment of a line names its hits; a line without one has none. Never built, never in the TU set (tidy_run.sh takes
// only tests/*.cpp), skipped by scripts/rules_lint.sh.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <err.h>
#include <error.h>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
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
template <class D>
struct CrtpBase
{
};
struct CrtpUser : CrtpBase<CrtpUser>       // custom-no-inheritance (CRTP)
{
};
template <class D>
struct CrtpMixin : CrtpBase<CrtpMixin<D>>  // custom-no-inheritance (a class template, never instantiated)
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

using IntBox = std::unique_ptr<int>;       // custom-no-smart-ptr (the written type)
typedef std::shared_ptr<int> IntShare;     // custom-no-smart-ptr (the written type)
template <class T>
using BoxOf = std::unique_ptr<T>;          // custom-no-smart-ptr (an alias template, where it is declared)
struct Holder
{
    std::unique_ptr<int> p;                // custom-no-smart-ptr x2 (the field's type, the written type)
    IntBox q;                              // custom-no-smart-ptr (through the alias; both at one column: one hit)
    IntShare s;                            // custom-no-smart-ptr (through the typedef)
    std::weak_ptr<int> w;                  // custom-no-smart-ptr x2 (weak_ptr)
};
std::vector<std::shared_ptr<int>>* g_list = nullptr;  // custom-no-global-var, custom-no-smart-ptr (a template argument)
std::unique_ptr<int> make_box();           // custom-no-smart-ptr x2 (the return type, the written type)
int peek( const std::shared_ptr<int>& sp );  // custom-no-smart-ptr x2 (a const& parameter, the written type)
template <class T>
int peek_t( const std::shared_ptr<T>& sp )  // custom-no-smart-ptr x2 (a dependent const& parameter, never instantiated)
{
    auto box = std::make_unique<T>();      // custom-no-smart-ptr (a dependent make_unique, never instantiated)
    return sp && box ? 1 : 0;
}
template <class T>
std::weak_ptr<T> watch_t();                // custom-no-smart-ptr x2 (a dependent return type)

// a project redeclaration of a C library function: the call's callee is this declaration, outside a system header
extern "C" int fputs( const char* s, FILE* f );
extern "C" char* strdup( const char* s ) noexcept;

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

void report( va_list ap )
{
    error_at_line( 0, 0, "f.cpp", 1, "x" );  // custom-no-printf (glibc <error.h>)
    vwarnx( "x", ap );                       // custom-no-printf (BSD <err.h>)
    verrx( 1, "x", ap );                     // custom-no-printf (BSD <err.h>)
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
    alignas( int ) unsigned char buf[sizeof( int )];
    const int* pn = new ( buf ) int( 7 );  // custom-no-new-delete (placement new)
    std::make_shared<int>( 6 );            // custom-no-smart-ptr (a discarded make_shared)
    const int k = 5;
    const int* q = const_cast<int*>( &k ); // custom-no-const-cast
    std::printf( "%d\n", v );              // custom-no-printf
    std::fwrite( "x", 1, 1, stdout );      // custom-no-printf (C stdio output)
    fputs( "y", stdout );                  // custom-no-printf (through the redeclaration above)
    std::free( strdup( "z" ) );            // custom-no-new-delete x2 (strdup through the redeclaration, free)
    char* text = nullptr;
    asprintf( &text, "%d", v );            // custom-no-printf and custom-no-new-delete (prints and allocates)
    std::cout << local;                    // custom-no-std-stream
    std::wcout << L"w";                    // custom-no-std-stream (the w- form)
    try
    {
        std::cout.flush();                 // custom-no-std-stream
    }
    catch ( ... )                          // custom-no-catch-all
    {
    }
    return *q + *pn + g_counter + static_cast<int>( g_list != nullptr ) + file_static_fn() + t_local + Iface {}.f()
           + WithStatic::sfn() + Derived {}.b + static_cast<int>( Holder {}.p != nullptr ) + tangled( 1, 1, 1 );
}

// Control flow: else-if ladders, empty loop bodies, loops without a real exit condition.
int ladders( int a )
{
    if ( a == 0 )
        return 1;
    else if ( a == 1 )                     // custom-no-else-if
        return 2;
    if ( a == 2 )
    {
        return 3;
    }
    else
    {                                      // custom-no-else-if (an else whose only statement is an if)
        if ( a == 3 )
            return 4;
    }
    return 0;
}
template <class T>
int kind_of()
{
    if constexpr ( sizeof( T ) == 1 )
        return 1;
    else if constexpr ( sizeof( T ) == 2 )  // custom-no-else-if (if constexpr too)
        return 2;
    else
        return 4;
}
void loops( int n, volatile int& sink )
{
    for ( ;; )                             // custom-no-infinite-loop (no condition)
    {
        if ( ++sink > n )
            break;
    }
    for ( ; true; )                        // custom-no-infinite-loop
    {
        if ( ++sink > n )
            break;
    }
    while ( true )                         // custom-no-infinite-loop
    {
        if ( ++sink > n )
            break;
    }
    while ( 1 )                            // custom-no-infinite-loop (a non-zero integer literal)
    {
        if ( ++sink > n )
            break;
    }
    do                                     // custom-no-infinite-loop (in parentheses)
    {
        if ( ++sink > n )
            break;
    } while ( ( true ) );
    for ( int i = 0; i < n; ++i ) {}       // custom-no-empty-loop
    while ( ++sink < n );                  // custom-no-empty-loop (a ';' body)
    while ( ++sink < n )                   // custom-no-empty-loop (a comment is not a statement)
    {
        // nothing
    }
    int arr[3] = {};
    for ( int& v : arr );                  // custom-no-empty-loop (a range-for)
    do {} while ( ++sink < n );            // custom-no-empty-loop
}

// Near misses: none of the lines below may hit. An extra hit fails the self-test like a missing one.
constexpr int kLimit = 7;  // a constexpr global: not mutable
const int kConstGlobal = 8;  // a const global: not mutable
namespace
{
// a project error() in an anonymous namespace: ::error matches it by name, the C library filter keeps it out
int error( const char* what )
{
    return what != nullptr ? 1 : 0;
}
}  // namespace

int near_misses()
{
    std::string s;
    std::istringstream in( "a" );
    std::getline( in, s );  // std::getline, not the C library ::getline
    auto lam = []( int x ) { return x + 1; };
    int ( *fp )( int ) = lam;  // the conversion instantiates the implicit static __invoke of a capture-less lambda
    return error( "x" ) + kLimit + kConstGlobal + fp( 1 );
}

int quiet_flow( int n, volatile int& sink )
{
    if ( n == 0 )
        return 0;
    if ( n == 1 )  // independent ifs: not a ladder
        return 1;
    if ( n > 5 )
    {
        sink = 1;
    }
    else  // an else with more than an if in it
    {
        sink = 2;
        if ( n == 3 )
            sink = 3;
    }
    while ( sink < n )  // a real condition and a body
        sink = sink + 1;
    do
    {
        sink = 4;
    } while ( 0 );  // a false literal: runs once
    switch ( n )
    {
        case 2: return 2;
        default: break;
    }
    return sink;
}
