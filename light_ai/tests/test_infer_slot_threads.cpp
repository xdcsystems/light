// test_infer_slot_threads.cpp: TSAN coverage for InferSlot threaded paths

#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "infer_slot.hpp"
#include "suites.hpp"

namespace
{
    using namespace std::chrono_literals;

    InferJob make_job( int frame )
    {
        InferJob j;
        j.frame = frame;
        j.rga_ok = true;
        return j;
    }
}  // namespace

void test_infer_slot_threads( TestRun& t )
{
    InferSlot slot;
    constexpr int rounds = 1000;

    std::thread worker( [&] {
        slot.run( []( const InferJob& j, InferOutput& out ) {
            out.dets.clear();
            out.npu_ok = j.rga_ok;
            if ( j.rga_ok )
                out.dets.push( Detection { static_cast<float>( j.frame ), 0.0f, 1.0f, 1.0f, 0, 1.0f } );
            out.infer_ms = 1.0;
            out.post_ms = 0.5;
        } );
    } );

    int completed = 0;
    for ( int i = 0; i < rounds; ++i )
    {
        CHECK( t, slot.submit( make_job( i ) ) );
        InferResult res;
        const InferSlot::State st = slot.take( 5000ms, res );
        CHECK( t, st == InferSlot::State::Done );
        CHECK_EQ( t, res.job.frame, i );
        CHECK( t, res.npu_ok );
        CHECK_EQ( t, res.dets.n, 1 );
        CHECK_NEAR( t, res.dets.d[0].x, static_cast<float>( i ), 0.001f );
        ++completed;
    }
    CHECK_EQ( t, completed, rounds );

    CHECK( t, slot.submit( make_job( 1001 ) ) );
    CHECK( t, slot.reclaim( 5000ms ) );
    CHECK( t, slot.state() == InferSlot::State::Free );

    slot.close();
    worker.join();
    CHECK( t, !slot.submit( make_job( 1002 ) ) );

    InferSlot wedged_slot;
    std::atomic<bool> hold_worker { true };
    std::atomic<bool> worker_started { false };

    std::thread wedged_worker( [&] {
        wedged_slot.run( [&]( const InferJob&, InferOutput& out ) {
            worker_started.store( true );
            while ( hold_worker.load() )
                std::this_thread::sleep_for( 100us );
            out.npu_ok = true;
        } );
    } );

    CHECK( t, wedged_slot.submit( make_job( 42 ) ) );
    while ( !worker_started.load() )
        std::this_thread::yield();

    InferResult res;
    const InferSlot::State seen = wedged_slot.take( 1ms, res );
    CHECK( t, seen == InferSlot::State::Queued );
    CHECK( t, wedged_slot.state() == InferSlot::State::Abandoned );
    CHECK( t, !wedged_slot.submit( make_job( 43 ) ) );

    hold_worker.store( false );
    while ( wedged_slot.state() != InferSlot::State::Free )
        std::this_thread::yield();

    CHECK( t, wedged_slot.state() == InferSlot::State::Free );
    wedged_slot.close();
    wedged_worker.join();
}
