// Suite table runner shared by light_ai_tests (test_main.cpp) and light_ai_tests_tsan (tsan_main.cpp).
//   <prog> [substr...] [--repeat N]
// - substr: run only the suites whose name contains one of the substrings (no substring: every suite);
//   a substring that matches no suite is an error;
// - --repeat N (or --repeat=N), N >= 1: run the selection N times, each run on a fresh TestRun.
// Per run: one "ok    <suite> (N checks)" / "FAIL  <suite> ..." line per suite and the summary
// "<prog>: N checks, F failed"; with N > 1 a final "<prog>: total over R runs: ..." sums the runs.
// Exit: 0 no failure, 1 a failure in any run, 2 a bad argument (nothing runs).
// Each main keeps its table as a local array (no statics); plan_runs is pure, the harness self-test covers it.

#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <ostream>
#include <string>
#include <string_view>

#include "check.hpp"

struct Suite
{
    const char* name;
    void ( *run )( TestRun& );
};

template <std::size_t N>
struct RunPlan
{
    int repeat = 1;
    std::array<bool, N> selected {};
    std::string error;  // non-empty: a bad argument, exit 2
};

namespace suite_runner_detail
{
    constexpr long kMaxRepeat = 1000000;

    // The count of --repeat: a whole decimal number in 1..kMaxRepeat.
    inline bool parse_repeat( const char* s, int& out )
    {
        if ( s == nullptr || *s == '\0' )
            return false;
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol( s, &end, 10 );
        if ( errno != 0 || *end != '\0' || v < 1 || v > kMaxRepeat )
            return false;
        out = static_cast<int>( v );
        return true;
    }

    // Marks every suite whose name contains `sub`; false when none does.
    template <std::size_t N>
    bool select( const Suite ( &suites )[N], std::string_view sub, RunPlan<N>& plan )
    {
        bool hit = false;
        for ( std::size_t k = 0; k < N; ++k )
        {
            if ( std::string_view( suites[k].name ).find( sub ) == std::string_view::npos )
                continue;
            plan.selected[k] = true;
            hit = true;
        }
        return hit;
    }

    // --repeat at argv[i] (its count in argv[i + 1], consumed) or --repeat=N; returns the error text, empty if fine.
    inline std::string take_repeat( int argc, const char* const* argv, int& i, int& repeat )
    {
        const std::string_view arg = argv[i];
        const char* count = nullptr;
        if ( arg == "--repeat" )
        {
            if ( i + 1 >= argc )
                return "--repeat needs a count";
            count = argv[++i];
        }
        else
            count = argv[i] + std::string_view( "--repeat=" ).size();
        if ( !parse_repeat( count, repeat ) )
            return "invalid count for --repeat: '" + std::string( count ) + "' (expected 1.." + std::to_string( kMaxRepeat ) + ")";
        return {};
    }
}  // namespace suite_runner_detail

template <std::size_t N>
RunPlan<N> plan_runs( const Suite ( &suites )[N], int argc, const char* const* argv )
{
    RunPlan<N> plan;
    bool filtered = false;
    for ( int i = 1; i < argc && plan.error.empty(); ++i )
    {
        const std::string_view arg = argv[i];
        if ( arg == "--repeat" || arg.substr( 0, 9 ) == "--repeat=" )
        {
            plan.error = suite_runner_detail::take_repeat( argc, argv, i, plan.repeat );
            continue;
        }
        if ( arg.substr( 0, 1 ) == "-" )
        {
            plan.error = "unknown option '" + std::string( arg ) + "'";
            continue;
        }
        if ( !suite_runner_detail::select( suites, arg, plan ) )
        {
            plan.error = "no suite matches '" + std::string( arg ) + "'";
            continue;
        }
        filtered = true;
    }
    if ( !filtered )
        plan.selected.fill( true );
    return plan;
}

// Runs the plan: suite lines and summaries to `out`, failed checks to `err`. Returns the exit code.
template <std::size_t N>
int run_suites( const char* prog, const Suite ( &suites )[N], int argc, const char* const* argv, std::ostream& out,
        std::ostream& err )
{
    const RunPlan<N> plan = plan_runs( suites, argc, argv );
    if ( !plan.error.empty() )
    {
        err << prog << ": " << plan.error << '\n' << "usage: " << prog << " [substr...] [--repeat N]\n";
        return 2;
    }
    long total_checks = 0;
    long total_failures = 0;
    int failed_runs = 0;
    for ( int r = 0; r < plan.repeat; ++r )
    {
        TestRun t( err );
        for ( std::size_t k = 0; k < N; ++k )
        {
            if ( !plan.selected[k] )
                continue;
            const int checks0 = t.checks();
            const int fails0 = t.failures();
            suites[k].run( t );
            out << ( t.failures() == fails0 ? "ok    " : "FAIL  " ) << suites[k].name << " (" << t.checks() - checks0 << " checks)\n";
        }
        out << prog << ": " << t.checks() << " checks, " << t.failures() << " failed\n";
        total_checks += t.checks();
        total_failures += t.failures();
        failed_runs += t.failures() != 0 ? 1 : 0;
    }
    if ( plan.repeat > 1 )
        out << prog << ": total over " << plan.repeat << " runs: " << total_checks << " checks, " << total_failures << " failed, "
            << failed_runs << " runs failed\n";
    return total_failures == 0 ? 0 : 1;
}
