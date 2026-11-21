#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "core/reid_queue.hpp"
#include "suites.hpp"

namespace
{
    template <class Q>
    int drain_ready( Q& q ) noexcept
    {
        int n = 0;
        ReidResult res {};
        while ( q.poll_result( res ) )
        {
            if ( res.ok && res.dim == 4 )
                ++n;
        }
        return n;
    }
}  // namespace

void test_reid_worker_tsan( TestRun& t )
{
    struct FakeRunner
    {
        std::atomic<int>* processed = nullptr;
        bool operator()( const ReidJob& job, ReidResult& res ) const noexcept
        {
            res.dim = 4;
            res.feat[0] = static_cast<float>( job.local_id );
            res.feat[1] = 1.0f;
            l2_normalize( res.feat.data(), 4 );
            if ( processed )
                processed->fetch_add( 1, std::memory_order_relaxed );
            return true;
        }
    };

    std::atomic<int> processed { 0 };
    int submitted = 0;
    int collected = 0;
    {
        BasicReidQueue<FakeRunner> q( FakeRunner { &processed } );
        for ( int iter = 0; iter < 60; ++iter )
        {
            const int slot = q.try_acquire_slot();
            if ( slot >= 0 )
            {
                const ReidJob job { slot, iter % 2, ( iter % 5 ) + 1, static_cast<uint32_t>( iter + 1 ),
                                    static_cast<int64_t>( iter ) * 1000 };
                if ( q.submit( job ) )
                    ++submitted;
            }
            collected += drain_ready( q );
            if ( iter % 7 == 0 )
                std::this_thread::sleep_for( std::chrono::microseconds( 200 ) );
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
        while ( processed.load( std::memory_order_relaxed ) < submitted &&
                std::chrono::steady_clock::now() < deadline )
        {
            const int got = drain_ready( q );
            if ( got > 0 )
                collected += got;
            else
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        collected += drain_ready( q );
    }
    CHECK( t, submitted > 0 );
    CHECK( t, processed.load( std::memory_order_relaxed ) == submitted );
    CHECK( t, collected > 0 && collected <= submitted );
}
