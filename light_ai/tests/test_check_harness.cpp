// The harness itself (check.hpp, suite_runner.hpp): the exact failure texts of CHECK_EQ / CHECK_STR_EQ / CHECK_NEAR
// (an inner TestRun over an ostringstream), silence on a pass, each argument evaluated once, the counters;
// plan_runs / run_suites over a fake table (filter, --repeat, the argument errors and exit codes).
// The counters are checked with plain CHECK: a CHECK_STR_EQ that wrongly passed would show there.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "check.hpp"
#include "suite_runner.hpp"
#include "suites.hpp"

namespace
{
    // The head of a failure report for a check on `line` of this file.
    std::string failed_at( int line )
    {
        return std::string( __FILE__ ) + ':' + std::to_string( line ) + ": FAILED: ";
    }

    // The text written so far, then an empty stream for the next case.
    std::string take( std::ostringstream& out )
    {
        std::string s = out.str();
        out.str( {} );
        return s;
    }

    enum class Colour : std::uint8_t
    {
        red = 3,
        green = 7
    };

    void passing_checks( TestRun& t )
    {
        std::ostringstream out;
        TestRun inner( out );
        CHECK( inner, true );
        CHECK_EQ( inner, 2 + 2, 4 );
        CHECK_EQ( inner, Colour::red, Colour::red );
        CHECK_STR_EQ( inner, std::string( "ab" ), "ab" );
        CHECK_STR_EQ( inner, "", std::string() );
        CHECK_NEAR( inner, 0.5f, 0.5f + 1e-7f, 1e-6f );
        CHECK( t, inner.checks() == 6 && inner.failures() == 0 );
        CHECK( t, out.str().empty() );
    }

    void eq_failures( TestRun& t )
    {
        std::ostringstream out;
        TestRun inner( out );
        int line = __LINE__ + 1;
        CHECK_EQ( inner, 2 + 1, 4 );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "2 + 1 == 4\n  left:  3\n  right: 4\n" );
        CHECK( t, inner.checks() == 1 && inner.failures() == 1 );

        // Each argument once, also when the failure prints it.
        int i = 0;
        CHECK_EQ( inner, ++i, 1 );
        CHECK( t, i == 1 && out.str().empty() );
        line = __LINE__ + 1;
        CHECK_EQ( inner, ++i, 5 );
        CHECK( t, i == 2 );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "++i == 5\n  left:  2\n  right: 5\n" );

        // uint8_t and an enum as numbers, bool as a word, a plain char quoted and escaped.
        const std::uint8_t b = 200;
        line = __LINE__ + 1;
        CHECK_EQ( inner, b, std::uint8_t( 201 ) );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "b == std::uint8_t( 201 )\n  left:  200\n  right: 201\n" );
        line = __LINE__ + 1;
        CHECK_EQ( inner, Colour::red, Colour::green );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "Colour::red == Colour::green\n  left:  3\n  right: 7\n" );
        line = __LINE__ + 1;
        CHECK_EQ( inner, 1 > 2, true );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "1 > 2 == true\n  left:  false\n  right: true\n" );
        line = __LINE__ + 1;
        CHECK_EQ( inner, 'a', '\n' );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "'a' == '\\n'\n  left:  'a'\n  right: '\\n'\n" );

        // Floating point with every significant digit.
        line = __LINE__ + 1;
        CHECK_EQ( inner, 0.1f, 0.25f );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "0.1f == 0.25f\n  left:  0.100000001\n  right: 0.25\n" );
        line = __LINE__ + 1;
        CHECK_EQ( inner, 0.1, 0.25 );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "0.1 == 0.25\n  left:  0.10000000000000001\n  right: 0.25\n" );
        CHECK( t, inner.checks() == 9 && inner.failures() == 8 );
    }

    void str_failures( TestRun& t )
    {
        std::ostringstream out;
        TestRun inner( out );

        // A multi-line text: one line per side, the newlines escaped, the first difference located.
        const std::string got = "a: 1\nb: 2\n";
        int line = __LINE__ + 1;
        CHECK_STR_EQ( inner, got, "a: 1\nb: 3\n" );
        CHECK_STR_EQ( t, take( out ),
                failed_at( line ) + "got == \"a: 1\\nb: 3\\n\"\n  left:  \"a: 1\\nb: 2\\n\"\n  right: \"a: 1\\nb: 3\\n\"\n"
                                    "  first difference at index 8\n" );

        // One text a prefix of the other: the index is the shorter length.
        line = __LINE__ + 1;
        CHECK_STR_EQ( inner, "abc", "abcd" );
        CHECK_STR_EQ( t, take( out ),
                failed_at( line ) + "\"abc\" == \"abcd\"\n  left:  \"abc\"\n  right: \"abcd\"\n  first difference at index 3\n" );

        // \r\n against \n.
        line = __LINE__ + 1;
        CHECK_STR_EQ( inner, "x\r\n", "x\n" );
        CHECK_STR_EQ( t, take( out ),
                failed_at( line ) + "\"x\\r\\n\" == \"x\\n\"\n  left:  \"x\\r\\n\"\n  right: \"x\\n\"\n  first difference at index 1\n" );

        // A quote, a backslash, a tab and control bytes are escaped.
        const std::string odd = std::string( "\"\\\t" ) + '\x01' + '\x7f';
        line = __LINE__ + 1;
        CHECK_STR_EQ( inner, odd, "" );
        CHECK_STR_EQ( t, take( out ),
                failed_at( line ) + "odd == \"\"\n  left:  \"\\\"\\\\\\t\\x01\\x7f\"\n  right: \"\"\n  first difference at index 0\n" );

        // A null const char* prints (null), equals nothing (not even another null), has no index line.
        const char* none = nullptr;
        line = __LINE__ + 1;
        CHECK_STR_EQ( inner, none, "" );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "none == \"\"\n  left:  (null)\n  right: \"\"\n" );
        CHECK_STR_EQ( inner, none, none );
        CHECK( t, !take( out ).empty() );

        // Each argument once.
        int calls = 0;
        auto text = [&calls] {
            ++calls;
            return std::string( "z" );
        };
        CHECK_STR_EQ( inner, text(), "z" );
        CHECK( t, calls == 1 && out.str().empty() );
        CHECK_STR_EQ( inner, text(), "y" );
        CHECK( t, calls == 2 && !take( out ).empty() );
        CHECK( t, inner.checks() == 8 && inner.failures() == 7 );
    }

    void near_failures( TestRun& t )
    {
        std::ostringstream out;
        TestRun inner( out );
        const int line = __LINE__ + 1;
        CHECK_NEAR( inner, 1.0, 1.5, 0.25 );
        CHECK_STR_EQ( t, take( out ), failed_at( line ) + "1.0 ~= 1.5 (eps 0.25)\n  left:  1\n  right: 1.5\n  diff:  0.5\n" );

        // The bound is inclusive, as before; a NaN fails.
        CHECK_NEAR( inner, 1.0, 1.5, 0.5 );
        CHECK( t, out.str().empty() );
        CHECK_NEAR( inner, std::nan( "" ), 0.0, 1.0 );
        CHECK( t, !take( out ).empty() );

        // Each argument once.
        double x = 0.0;
        double e = 0.0;
        CHECK_NEAR( inner, x += 1.0, 1.0, e += 1e-9 );
        CHECK( t, x == 1.0 && e == 1e-9 && out.str().empty() );
        CHECK( t, inner.checks() == 4 && inner.failures() == 2 );
    }

    // The fake table of the runner checks.
    void fake_pass( TestRun& t ) { CHECK( t, true ); }
    void fake_fail( TestRun& t ) { CHECK( t, false ); }

    template <std::size_t M>
    RunPlan<3> plan( const Suite ( &fakes )[3], const char* const ( &args )[M] )
    {
        return plan_runs( fakes, static_cast<int>( M ), args );
    }

    bool selected( const RunPlan<3>& p, bool a, bool b, bool c )
    {
        return p.selected[0] == a && p.selected[1] == b && p.selected[2] == c;
    }

    void plan_checks( TestRun& t, const Suite ( &fakes )[3] )
    {
        const char* const none[] = { "prog" };
        RunPlan<3> p = plan( fakes, none );
        CHECK( t, p.error.empty() && p.repeat == 1 && selected( p, true, true, true ) );

        const char* const beta[] = { "prog", "beta" };
        p = plan( fakes, beta );
        CHECK( t, p.error.empty() && p.repeat == 1 && selected( p, false, true, true ) );

        const char* const repeat_log[] = { "prog", "--repeat", "3", "log" };
        p = plan( fakes, repeat_log );
        CHECK( t, p.error.empty() && p.repeat == 3 && selected( p, true, false, false ) );

        const char* const eq_form[] = { "prog", "threads", "--repeat=2", "alpha" };
        p = plan( fakes, eq_form );
        CHECK( t, p.error.empty() && p.repeat == 2 && selected( p, true, false, true ) );

        const char* const no_match[] = { "prog", "beta", "xyz" };
        CHECK_STR_EQ( t, plan( fakes, no_match ).error, "no suite matches 'xyz'" );
        const char* const no_count[] = { "prog", "--repeat" };
        CHECK_STR_EQ( t, plan( fakes, no_count ).error, "--repeat needs a count" );
        const char* const zero[] = { "prog", "--repeat", "0" };
        CHECK_STR_EQ( t, plan( fakes, zero ).error, "invalid count for --repeat: '0' (expected 1..1000000)" );
        const char* const word[] = { "prog", "--repeat", "x" };
        CHECK_STR_EQ( t, plan( fakes, word ).error, "invalid count for --repeat: 'x' (expected 1..1000000)" );
        const char* const tail[] = { "prog", "--repeat", "3x" };
        CHECK_STR_EQ( t, plan( fakes, tail ).error, "invalid count for --repeat: '3x' (expected 1..1000000)" );
        const char* const empty_eq[] = { "prog", "--repeat=" };
        CHECK_STR_EQ( t, plan( fakes, empty_eq ).error, "invalid count for --repeat: '' (expected 1..1000000)" );
        const char* const too_many[] = { "prog", "--repeat", "1000001" };
        CHECK_STR_EQ( t, plan( fakes, too_many ).error, "invalid count for --repeat: '1000001' (expected 1..1000000)" );
        const char* const option[] = { "prog", "--list" };
        CHECK_STR_EQ( t, plan( fakes, option ).error, "unknown option '--list'" );
    }

    template <std::size_t M>
    int run( const Suite ( &fakes )[3], const char* const ( &args )[M], std::ostringstream& out, std::ostringstream& err )
    {
        out.str( {} );
        err.str( {} );
        return run_suites( "prog", fakes, static_cast<int>( M ), args, out, err );
    }

    void run_checks( TestRun& t, const Suite ( &fakes )[3] )
    {
        std::ostringstream out;
        std::ostringstream err;

        // --repeat 3 log: the selected suite three times, a summary per run, then the total.
        const char* const repeat_log[] = { "prog", "--repeat", "3", "log" };
        CHECK_EQ( t, run( fakes, repeat_log, out, err ), 0 );
        const std::string one = "ok    alpha_log (1 checks)\nprog: 1 checks, 0 failed\n";
        CHECK_STR_EQ( t, out.str(), one + one + one + "prog: total over 3 runs: 3 checks, 0 failed, 0 runs failed\n" );
        CHECK( t, err.str().empty() );

        // One run: no total line; a failed suite is FAIL, its check goes to err, exit 1.
        const char* const beta[] = { "prog", "beta" };
        CHECK_EQ( t, run( fakes, beta, out, err ), 1 );
        CHECK_STR_EQ( t, out.str(), "ok    beta (1 checks)\nFAIL  beta_threads (1 checks)\nprog: 2 checks, 1 failed\n" );
        CHECK( t, err.str().find( ": FAILED: false\n" ) != std::string::npos );

        // Fresh counters per run; the total sums them and counts the failed runs.
        const char* const repeat_fail[] = { "prog", "--repeat", "2", "threads" };
        CHECK_EQ( t, run( fakes, repeat_fail, out, err ), 1 );
        const std::string bad = "FAIL  beta_threads (1 checks)\nprog: 1 checks, 1 failed\n";
        CHECK_STR_EQ( t, out.str(), bad + bad + "prog: total over 2 runs: 2 checks, 2 failed, 2 runs failed\n" );

        // A bad argument: the error and the usage line, nothing runs, exit 2.
        const char* const no_match[] = { "prog", "xyz" };
        CHECK_EQ( t, run( fakes, no_match, out, err ), 2 );
        CHECK( t, out.str().empty() );
        CHECK_STR_EQ( t, err.str(), "prog: no suite matches 'xyz'\nusage: prog [substr...] [--repeat N]\n" );
    }
}  // namespace

void test_check_harness( TestRun& t )
{
    passing_checks( t );
    eq_failures( t );
    str_failures( t );
    near_failures( t );

    const Suite fakes[] = {
        { "alpha_log", &fake_pass },
        { "beta", &fake_pass },
        { "beta_threads", &fake_fail },
    };
    plan_checks( t, fakes );
    run_checks( t, fakes );
}
