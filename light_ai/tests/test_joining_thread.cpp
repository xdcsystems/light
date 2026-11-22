// JoiningThread (include/platform/joining_thread.hpp): RAII join on destruction,
// idempotent join(), start() joining prior thread, move construction/assignment.

#include <atomic>
#include <utility>

#include "check.hpp"
#include "platform/joining_thread.hpp"
#include "suites.hpp"

void test_joining_thread( TestRun& t )
{
    // Default-constructed: not joinable, idempotent join().
    {
        JoiningThread th;
        CHECK( t, !th.joinable() );
        th.join();
        CHECK( t, !th.joinable() );
    }

    // RAII destructor joins without an explicit join() call.
    {
        std::atomic<int> hits { 0 };
        {
            const JoiningThread th( [&hits] { hits.fetch_add( 1, std::memory_order_relaxed ); } );
            CHECK( t, th.joinable() );
        }
        CHECK_EQ( t, hits.load( std::memory_order_relaxed ), 1 );
    }

    // start() joins the previous thread before launching the next one.
    {
        std::atomic<int> hits { 0 };
        JoiningThread th;
        th.start( [&hits] { hits.fetch_add( 1, std::memory_order_relaxed ); } );
        th.start( [&hits] { hits.fetch_add( 10, std::memory_order_relaxed ); } );
        th.join();
        th.join();  // idempotent
        CHECK( t, !th.joinable() );
        CHECK_EQ( t, hits.load( std::memory_order_relaxed ), 11 );
    }

    // Move construction and move assignment transfer ownership and join prior target.
    {
        std::atomic<int> hits { 0 };
        JoiningThread a( [&hits] { hits.fetch_add( 1, std::memory_order_relaxed ); } );
        JoiningThread b( std::move( a ) );
        CHECK( t, b.joinable() );

        JoiningThread c( [&hits] { hits.fetch_add( 2, std::memory_order_relaxed ); } );
        c = std::move( b );
        CHECK( t, c.joinable() );
        c.join();
        CHECK( t, !c.joinable() );
        CHECK_EQ( t, hits.load( std::memory_order_relaxed ), 3 );
    }
}
