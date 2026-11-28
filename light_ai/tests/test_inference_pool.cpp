// InferencePool / InferSlot with a fake engine: mode selection, core masks,
// per-source FIFO ordering, LbGeom slot affinity & rotation, the NPU-skip and
// NPU-failure paths, a wedged engine (bounded wait, abandon, worker frees the
// slot), and per-source discard.
// The fake engine replaces RknnEngine through the pool's template parameter --
// no virtuals, no SDK.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "config/config.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "npu/infer_slot.hpp"
#include "npu/inference_pool.hpp"
#include "suites.hpp"

namespace
{
    using namespace std::chrono_literals;

    // Shared state of the fake NPU: what the engines were asked to do and a
    // gate that holds run() to simulate a wedged runtime.
    class FakeNpu
    {
    public:
        void hold()
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            held_ = true;
        }
        void release()
        {
            {
                const std::lock_guard<std::mutex> lk( mx_ );
                held_ = false;
            }
            cv_.notify_all();
        }
        // Inside run(): blocks while held. Bounded, so a broken test cannot hang the suite.
        void gate() const
        {
            std::unique_lock<std::mutex> lk( mx_ );
            cv_.wait_for( lk, 5s, [this] { return !held_; } );
        }

        std::atomic<int> runs { 0 };
        std::atomic<int> run_rc { 0 };
        int setup_io_rc = 0;           // read on the constructing thread only
        std::vector<unsigned> masks;   // ditto
        int next_fd = 100;             // ditto

    private:
        mutable std::mutex mx_;
        mutable std::condition_variable cv_;
        bool held_ = false;
    };

    // The engine argument of the pool: FakeEngine writes the FakeNpu through it. Passed as a plain &npu, the
    // pointer reaches FakeEngine only through the const EngineArgs&... of the templated InferencePool constructor,
    // where misc-const-correctness does not look, and the check would call the FakeNpu const.
    FakeNpu* engine_arg( FakeNpu& npu ) { return &npu; }

    class FakeEngine
    {
    public:
        explicit FakeEngine( FakeNpu* npu ) : npu_( npu ) {}

        Status setup_io( bool = false, int input_slots = 1 )
        {
            if ( npu_->setup_io_rc != 0 )
                return make_status( Errc::Failed, "fake setup_io failure" );
            slots_.clear();
            slots_.reserve( static_cast<size_t>( input_slots ) );
            for ( int s = 0; s < input_slots; ++s )
                slots_.push_back( TensorRef { npu_->next_fd++, nullptr, 0, 1984 } );
            return {};
        }
        Status setup_io_shared( const TensorSet& shared_inputs, bool = false )
        {
            if ( npu_->setup_io_rc != 0 )
                return make_status( Errc::Failed, "fake setup_io_shared failure" );
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
        Status set_core_mask( unsigned mask )
        {
            npu_->masks.push_back( mask );
            return {};
        }
        Status run() const
        {
            npu_->gate();
            ++npu_->runs;
            return npu_->run_rc.load() == 0 ? Status{} : make_status( Errc::Failed, "fake run failure" );
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
        uint32_t input_stride_bytes() const noexcept { return 1984; }

    private:
        FakeNpu* npu_;
        std::vector<TensorRef> slots_;
        int bound_slot_ = 0;
    };

    // One detection that echoes the job: x = frame, y = source.
    struct FakeDecode
    {
        void operator()( const FakeEngine&, const InferJob& job, DetList& out ) const
        {
            out.push( Detection { static_cast<float>( job.frame ), static_cast<float>( job.src_idx ), 1.0f, 1.0f, 0, 0.9f } );
        }
    };

    class SelectiveGate
    {
    public:
        explicit SelectiveGate( int hold_frame ) : hold_frame_( hold_frame ) {}

        void release()
        {
            {
                const std::lock_guard<std::mutex> lk( mx_ );
                released_ = true;
            }
            cv_.notify_all();
        }

        void on_decode( int frame )
        {
            std::unique_lock<std::mutex> lk( mx_ );
            if ( frame == hold_frame_ && !released_ )
            {
                held_entered_ = true;
                cv_.notify_all();
                cv_.wait_for( lk, 5s, [this] { return released_; } );
            }
            done_frames_.push_back( frame );
            cv_.notify_all();
        }

        bool wait_held_entered()
        {
            std::unique_lock<std::mutex> lk( mx_ );
            return cv_.wait_for( lk, 2s, [this] { return held_entered_; } );
        }

        bool wait_frame_decoded( int frame )
        {
            std::unique_lock<std::mutex> lk( mx_ );
            return cv_.wait_for( lk, 2s, [this, frame] {
                for ( const int f : done_frames_ )
                {
                    if ( f == frame )
                        return true;
                }
                return false;
            } );
        }

    private:
        int hold_frame_;
        std::mutex mx_;
        std::condition_variable cv_;
        bool held_entered_ = false;
        bool released_ = false;
        std::vector<int> done_frames_;
    };

    struct GateDecode
    {
        SelectiveGate* gate = nullptr;

        void operator()( const FakeEngine&, const InferJob& job, DetList& out ) const
        {
            if ( gate != nullptr )
                gate->on_decode( job.frame );
            out.push( Detection { static_cast<float>( job.frame ), static_cast<float>( job.src_idx ), 1.0f, 1.0f, 0, 0.9f } );
        }
    };

    using Pool = InferencePool<FakeEngine, FakeDecode>;
    using GatePool = InferencePool<FakeEngine, GateDecode>;

    InferConfig pool_cfg( int pool, int core = 0 )
    {
        InferConfig c;
        c.pool = pool;
        c.core = core;
        return c;
    }

    PoolOptions fast_opts()
    {
        PoolOptions o;
        o.pin_workers = false;     // tests must not move the runner's threads
        o.result_timeout = 50ms;
        return o;
    }

    InferJob job( int frame, int src = 0, bool rga_ok = true, LbGeom lb = {} )
    {
        InferJob j;
        j.frame = frame;
        j.src_idx = src;
        j.rga_ok = rga_ok;
        j.lb = lb;
        j.tr.cam = 1.5;
        return j;
    }

    // Waits until the pool hands out a Free slot again (a worker freed an abandoned one).
    int wait_acquire( const Pool& p )
    {
        if ( p.wait_any_slot_free( 2000ms ) )
            return p.try_acquire();
        return -1;
    }

    bool wait_front_done( const GatePool& p )
    {
        for ( int i = 0; i < 400; ++i )
        {
            if ( p.front_done() )
                return true;
            std::this_thread::sleep_for( 2ms );
        }
        return false;
    }

    void modes_and_masks( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 0 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            CHECK( t, p.size() == 1 && p.workers_count() == 1 && npu.masks.empty() );  // AUTO: no core-mask call
            CHECK( t, p.tensor( 0 ).stride_bytes == 1984 );
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 1, 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            CHECK( t, p.size() == 1 && p.workers_count() == 1 && npu.masks == std::vector<unsigned> { 3U } );
            CHECK( t, out.str().find( "npu: core mask 0x3\n" ) != std::string::npos );
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 2 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            CHECK( t, p.size() == 1 && p.workers_count() == 1 );  // the untested 2-engine mode cannot be built
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 3, 7 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            CHECK( t, p.size() == 6 && p.workers_count() == 3 && npu.masks == ( std::vector<unsigned> { 1U, 2U, 4U } ) );
            const TensorSet ts = p.tensors();
            CHECK( t, ts.n == 6 && ts.d[0].fd == 100 && ts.d[1].fd == 101 && ts.d[2].fd == 102 &&
                      ts.d[3].fd == 103 && ts.d[4].fd == 104 && ts.d[5].fd == 105 );
            CHECK( t, p.empty() && !p.full() && p.try_acquire() == 0 );
        }
        {
            FakeNpu npu;
            npu.setup_io_rc = -1;
            bool threw = false;
            try
            {
                const Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            }
            catch ( const std::runtime_error& )
            {
                threw = true;
            }
            CHECK( t, threw );
        }
    }

    void in_order_results( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
        npu.hold();  // all six slots in flight at once: the FIFO, not the finish order, decides
        for ( int f = 10; f < 16; ++f )
        {
            const int k = p.try_acquire();
            CHECK( t, k >= 0 && p.submit( k, job( f, 0 ) ) );
        }
        CHECK( t, p.full() && p.pending() == 6 && p.try_acquire() == -1 );
        CHECK( t, !p.submit( 0, job( 99 ) ) );  // FIFO full: refused and logged
        npu.release();
        InferResult r;
        for ( int f = 10; f < 16; ++f )
        {
            CHECK( t, p.collect( r ) );
            CHECK( t, r.job.frame == f && r.job.src_idx == 0 && r.npu_ok );
            CHECK( t, r.dets.n == 1 && r.dets.d[0].x == static_cast<float>( f ) );
            CHECK( t, r.job.tr.cam == 1.5 && r.job.tr.infer >= 0.0 && r.slot >= 0 && r.slot < 6 );
        }
        CHECK( t, p.empty() && npu.runs.load() == 6 && p.try_acquire() >= 0 );
        CHECK( t, err.str().find( "pubfifo: overflow" ) != std::string::npos );
        CHECK( t, !p.collect( r ) && !r.npu_ok );  // nothing pending
    }

    void per_source_fifo( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        SelectiveGate gate( 100 );
        GatePool p( pool_cfg( 3 ), GateDecode { &gate }, log, fast_opts(), engine_arg( npu ) );

        // Hold s0 frame 100 on a worker, let s0 frame 101 and s1 frame 200 finish.
        CHECK( t, p.submit( p.try_acquire(), job( 100, 0 ) ) );
        CHECK( t, gate.wait_held_entered() );
        CHECK( t, p.submit( p.try_acquire(), job( 101, 0 ) ) );
        CHECK( t, gate.wait_frame_decoded( 101 ) );

        // s0 frame 101 is decoded, yet blocked behind s0 frame 100: front_done() stays false
        // until s1 frame 200 completes.
        CHECK( t, p.submit( p.try_acquire(), job( 200, 1 ) ) );
        CHECK( t, gate.wait_frame_decoded( 200 ) );
        CHECK( t, wait_front_done( p ) );

        // Ready s1 frame is collected without waiting for held s0 frame 100.
        InferResult r;
        CHECK( t, p.collect( r ) );
        CHECK_EQ( t, r.job.frame, 200 );
        CHECK_EQ( t, r.job.src_idx, 1 );
        CHECK( t, r.npu_ok );

        // Same-source order preserved: s0 frame 101 cannot jump ahead of held s0 frame 100.
        CHECK( t, !p.front_done() );

        gate.release();
        CHECK( t, p.collect( r ) );
        CHECK_EQ( t, r.job.frame, 100 );
        CHECK_EQ( t, r.job.src_idx, 0 );
        CHECK( t, p.collect( r ) );
        CHECK_EQ( t, r.job.frame, 101 );
        CHECK_EQ( t, r.job.src_idx, 0 );
        CHECK( t, p.empty() );
    }

    void lb_affinity_and_rotation( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );

        const LbGeom lb_land { 0, 640, 140, 360 };
        const LbGeom lb_port { 140, 360, 0, 640 };

        // Initial ties rotate 0 -> 1.
        const int s0 = p.try_acquire( 0, lb_land );
        CHECK_EQ( t, s0, 0 );
        CHECK( t, p.submit( s0, job( 1, 0, true, lb_land ) ) );

        const int s1 = p.try_acquire( 0, lb_port );
        CHECK_EQ( t, s1, 1 );
        CHECK( t, p.submit( s1, job( 2, 1, true, lb_port ) ) );

        InferResult r;
        CHECK( t, p.collect( r ) && p.collect( r ) );

        // next_slot_ is 2, slots 2..5 have empty last_lb_, slot 0 matches lb_land -> affinity picks 0.
        CHECK_EQ( t, p.try_acquire( 0, lb_land ), 0 );
        CHECK( t, p.submit( 0, job( 3, 0, true, lb_land ) ) );

        // Slot 0 is busy; no free slot matches lb_land, so rotation from next_slot_=1 picks 1.
        CHECK_EQ( t, p.try_acquire( 0, lb_land ), 1 );
        CHECK( t, p.collect( r ) );  // free slot 0 (next_slot_ is now 2)

        // Slot 0 matches lb_land, but is avoided while slots 2..5 are non-avoided -> picks 2.
        CHECK_EQ( t, p.try_acquire( 1u << 0, lb_land ), 2 );

        // Fill slots 2..5 so only avoided slots 0 and 1 remain free; lb_land affinity picks avoided 0 over 1.
        for ( int s = 2; s < 6; ++s )
            CHECK( t, p.submit( s, job( 10 + s, 0, true, lb_port ) ) );
        CHECK_EQ( t, p.try_acquire( ( 1u << 0 ) | ( 1u << 1 ), lb_land ), 0 );

        while ( !p.empty() )
            CHECK( t, p.collect( r ) );
    }

    void skip_and_failure( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
        InferResult r;
        // RGA failed: the NPU is skipped, the frame still comes back (timings count it).
        CHECK( t, p.submit( 0, job( 1, 0, false ) ) && p.collect( r ) );
        CHECK( t, !r.npu_ok && r.dets.n == 0 && r.job.tr.infer == 0.0 && npu.runs.load() == 0 );
        // rknn_run failed: no postprocess, npu_ok false.
        npu.run_rc = -1;
        CHECK( t, p.submit( 0, job( 2 ) ) && p.collect( r ) );
        CHECK( t, !r.npu_ok && r.dets.n == 0 && r.job.tr.post == 0.0 && npu.runs.load() == 1 );
        npu.run_rc = 0;
        CHECK( t, p.submit( 0, job( 3 ) ) && p.collect( r ) && r.npu_ok && r.dets.n == 1 );
    }

    // An index outside [0, size()): refused and logged, nothing queued.
    void submit_bad_slot( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
        CHECK( t, !p.submit( -1, job( 5 ) ) );
        CHECK( t, !p.submit( p.size(), job( 6 ) ) );
        CHECK( t, p.empty() && npu.runs.load() == 0 );
        CHECK( t, err.str() == "pool: invalid slot -1, frame 5 dropped\npool: invalid slot " + std::to_string( p.size() )
                                   + ", frame 6 dropped\n" );
        InferResult r;
        CHECK( t, p.submit( 0, job( 7 ) ) && p.collect( r ) && r.job.frame == 7 );
    }

    void wedged_engine( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
        npu.hold();
        CHECK( t, p.submit( 0, job( 7 ) ) );
        InferResult r;
        CHECK( t, !p.collect( r ) && !r.npu_ok );  // bounded wait gave up
        CHECK( t, err.str().find( "publish wait gave up on slot 0 (st=1), frame 7 dropped" ) != std::string::npos );
        // Abandoned, not freed: the tensor is still on the NPU.
        CHECK( t, p.empty() && p.try_acquire() == -1 && !p.wait_any_slot_free( 5ms ) );
        npu.release();
        CHECK( t, wait_acquire( p ) == 0 );  // the worker freed it and woke wait_any_slot_free
        CHECK( t, p.submit( 0, job( 8 ) ) && p.collect( r ) && r.job.frame == 8 && r.npu_ok );
    }

    void discard_by_source( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        {
            FakeNpu npu;
            Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            for ( int f = 1; f <= 3; ++f )
                CHECK( t, p.submit( p.try_acquire(), job( f, f == 2 ? 1 : 0 ) ) );
            p.discard( 0 );
            CHECK( t, p.pending() == 1 );
            InferResult r;
            CHECK( t, p.collect( r ) && r.job.frame == 2 && r.job.src_idx == 1 );
            CHECK( t, p.empty() && wait_acquire( p ) >= 0 );
        }
        {
            // Discarded frames still in flight are abandoned immediately (< 100 ms even with 5 s timeout).
            FakeNpu npu;
            PoolOptions long_opts = fast_opts();
            long_opts.result_timeout = std::chrono::milliseconds( 5000 );
            Pool p( pool_cfg( 3 ), FakeDecode {}, log, long_opts, engine_arg( npu ) );
            npu.hold();
            for ( int f = 5; f <= 10; ++f )
                CHECK( t, p.submit( p.try_acquire(), job( f, f == 6 ? 1 : 0 ) ) );
            const auto t0 = std::chrono::steady_clock::now();
            p.discard( 0 );
            const auto ms_el = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0 ).count();
            CHECK( t, ms_el < 100 && p.pending() == 1 && p.try_acquire() == -1 );
            npu.release();
            InferResult r;
            CHECK( t, p.collect( r ) && r.job.frame == 6 && r.job.src_idx == 1 );
            CHECK( t, p.empty() && wait_acquire( p ) >= 0 );
        }
    }

    void shutdown_with_work_in_flight( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        {
            Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), engine_arg( npu ) );
            for ( int f = 0; f < 6; ++f )
                CHECK( t, p.submit( p.try_acquire(), job( f ) ) );
        }  // destructor: closes the slots, joins the workers -- must return
        CHECK( t, npu.runs.load() <= 6 );
    }
}  // namespace

void test_inference_pool( TestRun& t )
{
    modes_and_masks( t );
    in_order_results( t );
    per_source_fifo( t );
    lb_affinity_and_rotation( t );
    skip_and_failure( t );
    submit_bad_slot( t );
    wedged_engine( t );
    discard_by_source( t );
    shutdown_with_work_in_flight( t );
}
