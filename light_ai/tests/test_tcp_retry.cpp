#include <atomic>
#include <cerrno>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/log.hpp"
#include "core/status.hpp"
#include "suites.hpp"
#include "net/tcp_pull_server.hpp"

void test_tcp_retry( TestRun& t )
{
    // Free port on the first try: no log line at all.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        const std::atomic<bool> quit { false };
        int calls = 0;
        const Status st = retry_listen( [&] { ++calls; return Status {}; }, quit, log, "enc", 1234 );
        CHECK( t, st.ok() );
        CHECK_EQ( t, calls, 1 );
        CHECK_EQ( t, err.str(), std::string() );
    }
    // Two failures, then Ok: the warn on the first failure only (state
    // change only); the 10 ms period keeps the suite fast, the text
    // derives from the period.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        const std::atomic<bool> quit { false };
        int calls = 0;
        const Status st = retry_listen(
            [&] {
                ++calls;
                return calls <= 2 ? errno_status( Errc::Busy, "bind", EADDRINUSE ) : Status {};
            },
            quit, log, "enc", 1234, 10 );
        CHECK( t, st.ok() );
        CHECK_EQ( t, calls, 3 );
        CHECK_STR_EQ( t, err.str(),
                      std::string( "enc: tcp listener on port 1234: bind: Address already in use; retrying every 10 ms\n" ) );
    }
    // The production cadence in the text: one failure, then Ok.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        const std::atomic<bool> quit { false };
        int calls = 0;
        const Status st = retry_listen(
            [&] {
                ++calls;
                return calls <= 1 ? errno_status( Errc::Busy, "bind", EADDRINUSE ) : Status {};
            },
            quit, log, "net", 1234 );
        CHECK( t, st.ok() );
        CHECK_EQ( t, calls, 2 );
        CHECK_STR_EQ( t, err.str(),
                      std::string( "net: tcp listener on port 1234: bind: Address already in use; retrying every 2 s\n" ) );
    }
    // Eternal failure, quit from another thread: the loop exits with the
    // failure well under a second (the wake slice is 100 ms).
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        std::atomic<bool> quit { false };
        int calls = 0;
        std::thread wake( [&quit] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
            quit.store( true );
        } );
        const auto t0 = std::chrono::steady_clock::now();
        const Status st = retry_listen(
            [&] {
                ++calls;
                return errno_status( Errc::Busy, "bind", EADDRINUSE );
            },
            quit, log, "enc", 1234 );
        const auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0 ).count();
        wake.join();
        CHECK( t, !st.ok() && st.code() == Errc::Busy );
        CHECK( t, el < 1000 );
        CHECK_EQ( t, calls, 1 );  // quit woke the wait before the first retry
    }
    // quit already set: one attempt still runs (the caller asked for one
    // bind), no warn -- the process is stopping.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        const std::atomic<bool> quit { true };
        int calls = 0;
        const Status st = retry_listen(
            [&] {
                ++calls;
                return errno_status( Errc::Busy, "bind", EADDRINUSE );
            },
            quit, log, "enc", 1234 );
        CHECK( t, !st.ok() );
        CHECK_EQ( t, calls, 1 );
        CHECK_EQ( t, err.str(), std::string() );
    }
}
