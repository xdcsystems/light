// InferencePool / InferSlot with a fake engine: mode selection, core masks,
// in-order results, the NPU-skip and NPU-failure paths, a wedged engine
// (bounded wait, abandon, the worker frees the slot) and per-source discard.
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
#include "infer_slot.hpp"
#include "inference_pool.hpp"
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

    class FakeEngine
    {
    public:
        explicit FakeEngine( FakeNpu* npu ) : npu_( npu ), fd_( npu->next_fd++ ) {}

        int setup_io() { return npu_->setup_io_rc; }
        int set_core_mask( unsigned mask )
        {
            npu_->masks.push_back( mask );
            return 0;
        }
        int run() const
        {
            npu_->gate();
            ++npu_->runs;
            return npu_->run_rc.load();
        }
        int input_fd() const { return fd_; }
        void* input_data() const { return nullptr; }
        uint32_t input_size() const { return 0; }

    private:
        FakeNpu* npu_;
        int fd_;
    };

    // One detection that echoes the job: x = frame, y = source.
    struct FakeDecode
    {
        void operator()( const FakeEngine&, const InferJob& job, DetList& out ) const
        {
            out.push( Detection { static_cast<float>( job.frame ), static_cast<float>( job.src_idx ), 1.0f, 1.0f, 0, 0.9f } );
        }
    };

    using Pool = InferencePool<FakeEngine, FakeDecode>;

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

    InferJob job( int frame, int src = 0, bool rga_ok = true )
    {
        InferJob j;
        j.frame = frame;
        j.src_idx = src;
        j.rga_ok = rga_ok;
        j.tr.cam = 1.5;
        return j;
    }

    // Polls until the pool hands out a Free slot again (a worker freed an abandoned one).
    int wait_acquire( const Pool& p )
    {
        for ( int i = 0; i < 400; ++i )
        {
            const int k = p.try_acquire();
            if ( k >= 0 )
                return k;
            std::this_thread::sleep_for( 5ms );
        }
        return -1;
    }

    void modes_and_masks( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 0 ), FakeDecode {}, log, fast_opts(), &npu );
            CHECK( t, p.size() == 1 && npu.masks.empty() );  // AUTO: no core-mask call
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 1, 3 ), FakeDecode {}, log, fast_opts(), &npu );
            CHECK( t, p.size() == 1 && npu.masks == std::vector<unsigned> { 3U } );
            CHECK( t, out.str().find( "npu: core mask 0x3\n" ) != std::string::npos );
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 2 ), FakeDecode {}, log, fast_opts(), &npu );
            CHECK( t, p.size() == 1 );  // the untested 2-engine mode cannot be built
        }
        {
            FakeNpu npu;
            const Pool p( pool_cfg( 3, 7 ), FakeDecode {}, log, fast_opts(), &npu );
            CHECK( t, p.size() == 3 && npu.masks == ( std::vector<unsigned> { 1U, 2U, 4U } ) );
            const TensorSet ts = p.tensors();
            CHECK( t, ts.n == 3 && ts.d[0].fd == 100 && ts.d[1].fd == 101 && ts.d[2].fd == 102 );
            CHECK( t, p.empty() && !p.full() && p.try_acquire() == 0 );
        }
        {
            FakeNpu npu;
            npu.setup_io_rc = -1;
            bool threw = false;
            try
            {
                const Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), &npu );
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
        Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), &npu );
        npu.hold();  // all three in flight at once: the FIFO, not the finish order, decides
        for ( int f = 10; f < 13; ++f )
        {
            const int k = p.try_acquire();
            CHECK( t, k >= 0 && p.submit( k, job( f, f - 10 ) ) );
        }
        CHECK( t, p.full() && p.pending() == 3 && p.try_acquire() == -1 );
        CHECK( t, !p.submit( 0, job( 99 ) ) );  // FIFO full: refused and logged
        npu.release();
        InferResult r;
        for ( int f = 10; f < 13; ++f )
        {
            CHECK( t, p.collect( r ) );
            CHECK( t, r.job.frame == f && r.job.src_idx == f - 10 && r.npu_ok );
            CHECK( t, r.dets.n == 1 && r.dets.d[0].x == static_cast<float>( f ) );
            CHECK( t, r.job.tr.cam == 1.5 && r.job.tr.infer >= 0.0 && r.slot >= 0 && r.slot < 3 );
        }
        CHECK( t, p.empty() && npu.runs.load() == 3 && p.try_acquire() >= 0 );
        CHECK( t, err.str().find( "pubfifo: overflow" ) != std::string::npos );
        CHECK( t, !p.collect( r ) && !r.npu_ok );  // nothing pending
    }

    void skip_and_failure( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), &npu );
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

    void wedged_engine( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), &npu );
        npu.hold();
        CHECK( t, p.submit( 0, job( 7 ) ) );
        InferResult r;
        CHECK( t, !p.collect( r ) && !r.npu_ok );  // bounded wait gave up
        CHECK( t, err.str().find( "publish wait gave up on slot 0 (st=1), frame 7 dropped" ) != std::string::npos );
        // Abandoned, not freed: the tensor is still on the NPU.
        CHECK( t, p.empty() && p.try_acquire() == -1 );
        npu.release();
        CHECK( t, wait_acquire( p ) == 0 );  // the worker freed it
        CHECK( t, p.submit( 0, job( 8 ) ) && p.collect( r ) && r.job.frame == 8 && r.npu_ok );
    }

    void discard_by_source( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        {
            FakeNpu npu;
            Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), &npu );
            for ( int f = 1; f <= 3; ++f )
                CHECK( t, p.submit( p.try_acquire(), job( f, f == 2 ? 1 : 0 ) ) );
            p.discard( 0 );
            CHECK( t, p.pending() == 1 );
            InferResult r;
            CHECK( t, p.collect( r ) && r.job.frame == 2 && r.job.src_idx == 1 );
            CHECK( t, p.empty() && wait_acquire( p ) >= 0 );
        }
        {
            // A discarded frame still on a wedged NPU is abandoned; its worker frees it.
            FakeNpu npu;
            Pool p( pool_cfg( 1 ), FakeDecode {}, log, fast_opts(), &npu );
            npu.hold();
            CHECK( t, p.submit( 0, job( 5 ) ) );
            p.discard( 0 );
            CHECK( t, p.empty() && p.try_acquire() == -1 );
            npu.release();
            CHECK( t, wait_acquire( p ) == 0 );
        }
    }

    void shutdown_with_work_in_flight( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeNpu npu;
        {
            Pool p( pool_cfg( 3 ), FakeDecode {}, log, fast_opts(), &npu );
            for ( int f = 0; f < 3; ++f )
                CHECK( t, p.submit( p.try_acquire(), job( f ) ) );
        }  // destructor: closes the slots, joins the workers -- must return
        CHECK( t, npu.runs.load() <= 3 );
    }
}  // namespace

void test_inference_pool( TestRun& t )
{
    modes_and_masks( t );
    in_order_results( t );
    skip_and_failure( t );
    wedged_engine( t );
    discard_by_source( t );
    shutdown_with_work_in_flight( t );
}
