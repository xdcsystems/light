#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <thread>
#include <vector>

#include "check.hpp"
#include "platform/pi_mutex.hpp"
#include "config/config.hpp"
#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "core/status.hpp"
#include "npu/infer_slot.hpp"
#include "npu/inference_pool.hpp"
#include "suites.hpp"

namespace
{
    using namespace std::chrono_literals;

    InferJob make_job( int frame, int src = 0 )
    {
        InferJob j;
        j.frame = frame;
        j.src_idx = src;
        j.rga_ok = true;
        return j;
    }

    class FakeNpu
    {
    public:
        void hold()
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            held_ = true;
        }

        void release()
        {
            {
                const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
                held_ = false;
            }
            cv_.notify_all();
        }

        bool wait_entered( int target, std::chrono::milliseconds timeout ) const
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
            return cv_.wait_for( lk, timeout, [this, target] { return entered_ >= target; } );
        }

        void gate()
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
            ++entered_;
            cv_.notify_all();
            cv_.wait_for( lk, 2000ms, [this] { return !held_; } );
        }

        std::atomic<int> runs { 0 };
        int next_fd = 100;

    private:
        mutable light_ai::platform::PiMutex mx_;
        mutable light_ai::platform::PiCondVar cv_;
        bool held_ = false;
        int entered_ = 0;
    };

    FakeNpu* engine_arg( FakeNpu& npu ) { return &npu; }

    class FakeEngine
    {
    public:
        explicit FakeEngine( FakeNpu* npu ) : npu_( npu ) {}
        Status setup_io( bool = false, int input_slots = 1 )
        {
            slots_.clear();
            slots_.reserve( static_cast<size_t>( input_slots ) );
            for ( int s = 0; s < input_slots; ++s )
                slots_.push_back( TensorRef { npu_->next_fd++, nullptr, 0, 1920 } );
            return {};
        }
        Status setup_io_shared( const TensorSet& shared_inputs, bool = false )
        {
            slots_.clear();
            for ( int s = 0; s < shared_inputs.n; ++s )
                slots_.push_back( shared_inputs.d[s] );
            return {};
        }
        Status bind_input_slot( int slot ) noexcept
        {
            if ( slot < 0 || slot >= static_cast<int>( slots_.size() ) )
                return make_status( Errc::Invalid, "invalid input slot" );
            bound_slot_ = slot;
            return {};
        }
        Status set_core_mask( unsigned ) { return {}; }
        Status run() const
        {
            npu_->gate();
            npu_->runs.fetch_add( 1, std::memory_order_relaxed );
            return {};
        }
        int input_fd() const { return slots_.empty() ? -1 : slots_[0].fd; }
        void* input_data() const { return nullptr; }
        uint32_t input_size() const { return 0; }
        TensorRef input_tensor( int slot ) const noexcept
        {
            if ( slot < 0 || slot >= static_cast<int>( slots_.size() ) )
                return {};
            return slots_[static_cast<size_t>( slot )];
        }
        uint32_t input_stride_bytes() const noexcept { return 1920; }

    private:
        FakeNpu* npu_;
        std::vector<TensorRef> slots_;
        int bound_slot_ = 0;
    };

    struct FakeDecode
    {
        void operator()( const FakeEngine&, const InferJob& job, DetList& out ) const
        {
            out.push( Detection { static_cast<float>( job.frame ), static_cast<float>( job.src_idx ), 1.0f, 1.0f, 0, 0.9f } );
        }
    };

    using Pool = InferencePool<FakeEngine, FakeDecode>;

    int wait_slot_zero_free( const Pool& p )
    {
        for ( int i = 0; i < 400; ++i )
        {
            const int k = p.try_acquire( 0x3Eu );
            if ( k == 0 )
                return k;
            std::this_thread::sleep_for( 5ms );
        }
        return -1;
    }

    void test_pool_abandoned_and_avoid_mask( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        InferConfig cfg {};
        cfg.pool = 3;
        PoolOptions opts {};
        opts.pin_workers = false;
        opts.result_timeout = 15ms;

        FakeNpu npu;
        Pool pool( cfg, FakeDecode {}, log, opts, engine_arg( npu ) );

        npu.hold();
        CHECK_EQ( t, pool.try_acquire( 0 ), 0 );
        CHECK( t, pool.submit( 0, make_job( 10, 0 ) ) );
        CHECK( t, npu.wait_entered( 1, 1000ms ) );

        InferResult res {};
        CHECK( t, !pool.collect( res ) );  // slot 0 -> Abandoned
        CHECK( t, pool.empty() );

        // Slot 0 is Abandoned, slot 1 is avoided -> must select slot 2.
        CHECK_EQ( t, pool.try_acquire( 1u << 1 ), 2 );
        CHECK( t, pool.submit( 2, make_job( 11, 0 ) ) );
        CHECK( t, npu.wait_entered( 2, 1000ms ) );
        for ( int s = 3; s < 6; ++s )
        {
            CHECK_EQ( t, pool.try_acquire( 1u << 1 ), s );
            CHECK( t, pool.submit( s, make_job( 9 + s, 0 ) ) );
        }
        CHECK( t, npu.wait_entered( 3, 1000ms ) );

        // Slot 0 is Abandoned, slots 2..5 are Queued, slot 1 is Free & avoided -> fallback to slot 1.
        CHECK_EQ( t, pool.try_acquire( 1u << 1 ), 1 );
        CHECK( t, pool.submit( 1, make_job( 15, 1 ) ) );
        CHECK_EQ( t, pool.try_acquire( 0 ), -1 );

        npu.release();
        int next_s0 = 11;
        bool got_s1 = false;
        for ( int c = 0; c < 5; ++c )
        {
            CHECK( t, pool.collect( res ) && res.npu_ok );
            if ( res.job.src_idx == 0 )
            {
                CHECK_EQ( t, res.job.frame, next_s0 );
                ++next_s0;
            }
            else
            {
                CHECK_EQ( t, res.job.src_idx, 1 );
                CHECK_EQ( t, res.job.frame, 15 );
                CHECK( t, !got_s1 );
                got_s1 = true;
            }
        }
        CHECK_EQ( t, next_s0, 15 );
        CHECK( t, got_s1 );
        CHECK_EQ( t, wait_slot_zero_free( pool ), 0 );

        // Concurrent avoid_mask + discard() stress across 6 slots and 3 worker threads.
        bool all_collect_ok = true;
        int collected = 0;
        for ( int i = 0; i < 60; ++i )
        {
            const uint32_t avoid = 1u << static_cast<unsigned>( i % 6 );
            int k = pool.try_acquire( avoid );
            while ( k < 0 )
            {
                if ( !pool.empty() )
                {
                    const bool ok = pool.collect( res );
                    all_collect_ok = all_collect_ok && ok;
                    collected += static_cast<int>( ok );
                }
                else
                {
                    std::this_thread::yield();
                }
                k = pool.try_acquire( avoid );
            }
            CHECK( t, pool.submit( k, make_job( 100 + i, i % 2 ) ) );
            if ( ( i % 15 ) == 14 )
                pool.discard( 1 );
            if ( pool.front_done() )
            {
                const bool ok = pool.collect( res );
                all_collect_ok = all_collect_ok && ok;
                collected += static_cast<int>( ok );
            }
        }
        while ( !pool.empty() )
        {
            const bool ok = pool.collect( res );
            all_collect_ok = all_collect_ok && ok;
            collected += static_cast<int>( ok );
        }
        CHECK( t, all_collect_ok );
        CHECK( t, collected > 0 );
        CHECK( t, npu.runs.load( std::memory_order_relaxed ) > 3 );
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

    test_pool_abandoned_and_avoid_mask( t );
}
