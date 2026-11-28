// InferencePool: the NPU stage -- up to 3 NPU workers fed by a shared work
// queue over up to 6 input tensor slots, and a per-source FIFO that hands
// results back in per-source submit order (a ready source never blocks behind
// another source's slow/wedged frame, while same-source frames stay ordered).
//
// Mode, picked once from InferConfig::pool:
//   pool < 3 -> 1 worker, 1 slot: the legacy synchronous path;
//   pool 3   -> 3 workers (one per NPU core), 6 tensor slots: overlapped
//               RGA resize, NPU inference, and output GPU/H.264 consumption.
// 2 is rejected by the CLI; anything below 3 maps to 1 worker / 1 slot.
//
// Threading: all public pipeline methods run on the pipeline thread -- fifo_,
// next_slot_, and last_lb_ have no lock by design. Workers pop slot indices
// from work_q_ (under work_mx_), bind the slot's tensor into their own engine,
// and run with their own Decode copy.
//
// Destruction stops the work queue, closes every slot, and joins the workers
// before the engines die.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "config/config.hpp"  // InferConfig
#include "core/frame_view.hpp"  // TensorRef, TensorSet
#include "core/log.hpp"
#include "core/non_copyable.hpp"
#include "core/pod_list.hpp"
#include "core/pod_ring.hpp"
#include "core/throw_on.hpp"
#include "core/timings.hpp"  // Timings::clk, ms()
#include "npu/dup_context_tag.hpp"
#include "npu/infer_slot.hpp"
#include "platform/joining_thread.hpp"
#include "platform/pi_mutex.hpp"
#include "sys/cpu_affinity.hpp"

struct PoolOptions
{
    bool pin_workers = true;  // pin each worker to the A76 cluster (cpu_affinity.hpp)
    // Bounded wait for one result (P0-2): rknn_run normally always
    // completes; running out of it means a wedged NPU/runtime -- the frame
    // is dropped instead of hanging the pipeline (and Ctrl-C) forever.
    std::chrono::milliseconds result_timeout { 2000 };
};

namespace inference_pool_detail
{
    // engines[0] owns the shared input dma-bufs: destroy the importers first, so their
    // rknn_destroy_mem() runs while the owner is still alive (vector's own order is front to back).
    template <typename Engine>
    void destroy_in_reverse( std::vector<Engine>& engines ) noexcept
    {
        while ( !engines.empty() )
            engines.pop_back();
    }
}  // namespace inference_pool_detail

template <typename Engine, typename Decode>
class InferencePool : private NonMovable<InferencePool<Engine, Decode>>
{
public:
    enum
    {
        kMaxWorkers = 3,
        kMaxSlots = 6
    };
    static_assert( static_cast<int>( kMaxSlots ) <= static_cast<int>( TensorSet::cap ), "TensorSet must cover every pool slot" );
    static_assert( kMaxSlots <= 32, "avoid_mask is a 32-bit slot bitmask" );

    // Loads the engines: engines_[0] from engine_args..., the others as duplicates
    // of it when Engine has a DupContextTag constructor, else from engine_args... too.
    // Sets up shared input tensors on engines_[0] and imports them into engines_[1..],
    // pins them to NPU cores, and starts the workers.
    // Throws std::runtime_error on any failure; nothing is left running then.
    template <typename... EngineArgs>
    InferencePool( const InferConfig& cfg, const Decode& decode, const Log& log, PoolOptions opts,
                   const EngineArgs&... engine_args )
      : decoders_ { { decode, decode, decode } }
      , log_( log )
      , opts_( opts )
      , n_workers_( cfg.pool >= kMaxWorkers ? kMaxWorkers : 1 )
      , n_slots_( n_workers_ >= kMaxWorkers ? kMaxSlots : 1 )
    {
        for ( int s = 0; s < n_slots_; ++s )
            slots_[static_cast<size_t>( s )].set_free_signal( &free_mx_, &free_cv_ );

        // A throw below skips the destructor body: the guard keeps its reverse destruction order.
        ReverseEnginesGuard guard( engines_ );
        engines_.reserve( static_cast<size_t>( n_workers_ ) );  // never reallocates after: &engines_[w] stays valid
        engines_.emplace_back( engine_args... );
        for ( int w = 1; w < n_workers_; ++w )
        {
            if constexpr ( std::is_constructible_v<Engine, DupContextTag, const Engine&, const Log&> )
            {
                engines_.emplace_back( DupContextTag {}, engines_[0], log_ );
            }
            else
            {
                engines_.emplace_back( engine_args... );
            }
        }
        throw_on( engines_[0].setup_io( false, n_slots_ ), "rknn setup_io" );
        const TensorSet shared = tensors();
        for ( int w = 1; w < n_workers_; ++w )
            throw_on( engines_[static_cast<size_t>( w )].setup_io_shared( shared, false ), "rknn setup_io_shared" );
        set_core_masks( cfg.core );
        start_workers();  // last: nothing below may throw with workers running
        guard.release();
    }

    ~InferencePool()
    {
        stop_workers();
        inference_pool_detail::destroy_in_reverse( engines_ );
    }

    int size() const { return n_slots_; }
    int workers_count() const { return n_workers_; }
    const Engine& engine( int k ) const { return engines_[static_cast<size_t>( k )]; }

    TensorRef tensor( int k ) const { return engines_[0].input_tensor( k ); }
    TensorSet tensors() const
    {
        TensorSet set {};
        for ( int k = 0; k < n_slots_; ++k )
            set.push( tensor( k ) );
        return set;
    }

    // --- pipeline thread -------------------------------------------------

    int pending() const { return fifo_.size(); }  // submitted, not collected yet
    bool empty() const { return fifo_.empty(); }
    bool full() const { return fifo_.size() >= n_slots_; }

    // True when any entry in fifo_ is both the oldest pending entry for its
    // source and in State::Done (collect() will return it without waiting).
    bool front_done() const { return find_ready_fifo_pos() >= 0; }

    // Blocks until at least one slot is Free (or closed), or timeout expires.
    bool wait_any_slot_free( std::chrono::milliseconds timeout ) const noexcept
    {
        std::unique_lock<light_ai::platform::PiMutex> lk( free_mx_ );
        return free_cv_.wait_for( lk, timeout, [this] { return any_slot_free_or_closed(); } );
    }

    // A Free slot whose tensor the caller may fill, or -1.
    // Preference: (!avoided && lb==eff_lb) > (!avoided) > (avoided && lb==eff_lb) > (avoided),
    // breaking ties by rotating from next_slot_ and advancing next_slot_ = (best + 1) % n_slots_.
    int try_acquire( uint32_t avoid_mask = 0, LbGeom eff_lb = {} ) const
    {
        int best_slot = -1;
        int best_tier = -1;
        for ( int off = 0; off < n_slots_; ++off )
        {
            const int i = ( next_slot_ + off ) % n_slots_;
            if ( slots_[static_cast<size_t>( i )].state() != InferSlot::State::Free )
                continue;
            const bool avoided = ( avoid_mask & ( 1u << static_cast<unsigned>( i ) ) ) != 0U;
            const bool lb_match = same_lb( last_lb_[static_cast<size_t>( i )], eff_lb );
            const int tier = ( avoided ? 0 : 2 ) + ( lb_match ? 1 : 0 );
            if ( tier > best_tier )
            {
                best_tier = tier;
                best_slot = i;
                if ( best_tier == 3 )
                    break;
            }
        }
        if ( best_slot >= 0 )
            next_slot_ = ( best_slot + 1 ) % n_slots_;
        return best_slot;
    }

    // Queues the frame whose pixels are already in tensor(k). false: the
    // frame is dropped (logged) -- k is outside [0, size()), the FIFO is full or slot k is not Free.
    bool submit( int k, const InferJob& job ) noexcept
    {
        if ( k < 0 || k >= n_slots_ )
        {
            log_.warn() << "pool: invalid slot " << k << ", frame " << job.frame << " dropped\n";
            return false;
        }
        if ( full() )
        {
            log_.warn() << "pubfifo: overflow (n=" << fifo_.size() << "), incoming slot " << k << " dropped\n";
            return false;
        }
        if ( !slots_[static_cast<size_t>( k )].submit( job ) )
        {
            log_.warn() << "pool: slot " << k << " is not free, frame " << job.frame << " dropped\n";
            return false;
        }
        last_lb_[static_cast<size_t>( k )] = job.lb;
        fifo_.push( k );
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( work_mx_ );
            static_cast<void>( work_q_.push( k ) );
        }
        work_cv_.notify_one();
        return true;
    }

    // Takes the oldest ready per-source result (or falls back to waiting on
    // fifo_.front() up to PoolOptions::result_timeout if none is ready yet).
    // true: `out` holds it and its slot is Free again. false: nothing was
    // pending, or the wait gave up (logged; the slot stays in flight until
    // a worker frees it) -- out.npu_ok is false either way.
    bool collect( InferResult& out ) noexcept
    {
        out.npu_ok = false;
        if ( fifo_.empty() )
        {
            log_.warn() << "pubfifo: empty (n=0), front invalid\n";
            return false;
        }
        const int ready_pos = find_ready_fifo_pos();
        const int k = pop_fifo_at( ready_pos >= 0 ? ready_pos : 0 );
        out.slot = k;
        const InferSlot::State seen = slots_[static_cast<size_t>( k )].take( opts_.result_timeout, out );
        if ( seen == InferSlot::State::Done )
            return true;
        log_.warn() << "pipeline: publish wait gave up on slot " << k << " (st=" << static_cast<int>( seen )
                    << "), frame " << out.job.frame << " dropped\n";
        return false;
    }

    // Drops every pending frame of src_idx immediately (its source detached:
    // those frames are never published). Frames of other sources keep their
    // order. Completed slots are freed; frames still in flight are marked
    // Abandoned with zero wait so their worker frees them upon completion.
    void discard( int src_idx )
    {
        const int n = fifo_.size();
        for ( int i = 0; i < n; ++i )
        {
            const int k = fifo_.front();
            fifo_.pop();
            InferSlot& slot = slots_[static_cast<size_t>( k )];
            if ( slot.src_idx() != src_idx )
            {
                fifo_.push( k );
                continue;
            }
            static_cast<void>( slot.reclaim( std::chrono::milliseconds::zero() ) );
        }
    }

private:
    // The destructor's order for a constructor that throws: destroys the engines in reverse unless released.
    class ReverseEnginesGuard : private NonMovable<ReverseEnginesGuard>
    {
    public:
        explicit ReverseEnginesGuard( std::vector<Engine>& engines ) noexcept : engines_( &engines ) {}
        ~ReverseEnginesGuard()
        {
            if ( engines_ != nullptr )
                inference_pool_detail::destroy_in_reverse( *engines_ );
        }
        void release() noexcept { engines_ = nullptr; }

    private:
        std::vector<Engine>* engines_;
    };

    bool same_lb( const LbGeom& a, const LbGeom& b ) const noexcept
    {
        return a.left == b.left && a.w == b.w && a.top == b.top && a.h == b.h;
    }

    bool any_slot_free_or_closed() const noexcept
    {
        for ( int i = 0; i < n_slots_; ++i )
        {
            const InferSlot& s = slots_[static_cast<size_t>( i )];
            if ( s.is_closed() || s.state() == InferSlot::State::Free )
                return true;
        }
        return false;
    }

    int fifo_at( int pos ) const noexcept
    {
        return fifo_.d[static_cast<size_t>( ( fifo_.head + pos ) % PodRing<int, kMaxSlots>::cap )];
    }

    bool contains_src( const PodList<int, kMaxSlots>& list, int src ) const noexcept
    {
        for ( int i = 0; i < list.n; ++i )
        {
            if ( list.d[static_cast<size_t>( i )] == src )
                return true;
        }
        return false;
    }

    int find_ready_fifo_pos() const noexcept
    {
        PodList<int, kMaxSlots> seen_src {};
        const int n = fifo_.size();
        for ( int i = 0; i < n; ++i )
        {
            const int k = fifo_at( i );
            int src = 0;
            const InferSlot::State st = slots_[static_cast<size_t>( k )].state_and_src( src );
            if ( contains_src( seen_src, src ) )
                continue;
            seen_src.push( src );
            if ( st == InferSlot::State::Done )
                return i;
        }
        return -1;
    }

    int pop_fifo_at( int target_pos ) noexcept
    {
        int popped = -1;
        const int n = fifo_.size();
        for ( int i = 0; i < n; ++i )
        {
            const int k = fifo_.front();
            fifo_.pop();
            if ( i == target_pos )
                popped = k;
            else
                fifo_.push( k );
        }
        return popped;
    }

    // Pool of one keeps cfg.core verbatim (0 = AUTO: no call -- the A/B
    // baseline is untouched); the 3-pool pins one engine per core.
    void set_core_masks( int core )
    {
        if ( n_workers_ == 1 )
        {
            if ( core != 0 )
            {
                throw_on( engines_[0].set_core_mask( static_cast<unsigned>( core ) ), "rknn set_core_mask" );
                log_.info() << "npu: core mask 0x" << std::hex << core << std::dec << "\n";
            }
            return;
        }
        constexpr std::array<unsigned, kMaxWorkers> core_masks { 1U, 2U, 4U };  // NPU core 0 / 1 / 2
        for ( int w = 0; w < n_workers_; ++w )
        {
            throw_on( engines_[static_cast<size_t>( w )].set_core_mask( core_masks[static_cast<size_t>( w )] ), "rknn set_core_mask" );
        }
        log_.info() << "npu: pool of " << n_workers_ << " engines (" << n_slots_ << " slots), one per core\n";
    }

    void start_workers()
    {
        try
        {
            for ( int w = 0; w < n_workers_; ++w )
                workers_[static_cast<size_t>( w )].start( &InferencePool::worker_loop, this, w );
        }
        catch ( ... )  // NOLINT(custom-no-catch-all): startup, a failed std::thread start; cleans up, then rethrows
        {
            // std::thread can fail to start (system_error): join the ones
            // already running, or their joinable handles would terminate().
            stop_workers();
            throw;
        }
    }

    void stop_workers()
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( work_mx_ );
            work_stop_ = true;
        }
        work_cv_.notify_all();
        for ( auto& slot : slots_ )
            slot.close();
        for ( int w = 0; w < n_workers_; ++w )
            workers_[static_cast<size_t>( w )].join();
    }

    bool next_work_slot( int& out_slot ) noexcept
    {
        std::unique_lock<light_ai::platform::PiMutex> lk( work_mx_ );
        work_cv_.wait( lk, [this] { return work_stop_ || !work_q_.empty(); } );
        if ( work_stop_ )
            return false;
        out_slot = work_q_.front();
        work_q_.pop();
        return true;
    }

    void worker_loop( int w ) noexcept
    {
        prefault_current_thread_stack();
        if ( opts_.pin_workers )
            pin_current_thread_rt( "npu worker", 5 + ( w % 3 ), 70, log_ );
        const std::size_t wi = static_cast<std::size_t>( w );
        Engine& eng = engines_[wi];
        Decode& dec = decoders_[wi];
        int k = -1;
        while ( next_work_slot( k ) )
        {
            InferSlot& slot = slots_[static_cast<std::size_t>( k )];
            InferJob job {};
            if ( !slot.begin_work( job ) )
                continue;
            InferOutput out {};
            if ( eng.bind_input_slot( k ).ok() )
                run_job( eng, dec, job, out );
            slot.finish_work( out );
        }
    }

    // One job on the worker thread: inference, then the postprocess.
    void run_job( const Engine& eng, Decode& dec, const InferJob& job, InferOutput& out ) const
    {
        out.dets.clear();
        out.npu_ok = false;
        out.infer_ms = 0.0;
        out.post_ms = 0.0;
        if ( !job.rga_ok )
            return;  // the tensor holds no valid frame: the NPU is skipped
        const auto t0 = Timings::clk::now();
        const bool ran = eng.run().ok();
        out.infer_ms = ms( t0, Timings::clk::now() );
        if ( !ran )
            return;
        const auto t1 = Timings::clk::now();
        dec( eng, job, out.dets );
        out.post_ms = ms( t1, Timings::clk::now() );
        out.npu_ok = true;
    }

private:
    static_assert( kMaxWorkers == 3, "decoders_ is initialized with one copy per worker" );
    std::array<Decode, kMaxWorkers> decoders_;  // one per worker: worker w touches only decoders_[w]
    const Log& log_;
    const PoolOptions opts_;
    const int n_workers_;
    const int n_slots_;
    std::vector<Engine> engines_;
    std::array<InferSlot, kMaxSlots> slots_ {};
    mutable light_ai::platform::PiMutex free_mx_;
    mutable light_ai::platform::PiCondVar free_cv_;
    light_ai::platform::PiMutex work_mx_;
    light_ai::platform::PiCondVar work_cv_;
    PodRing<int, kMaxSlots> work_q_ {};
    bool work_stop_ = false;
    PodRing<int, kMaxSlots> fifo_ {};  // slot indices in submit order; pipeline thread only
    mutable int next_slot_ = 0;
    std::array<LbGeom, kMaxSlots> last_lb_ {};
    std::array<JoiningThread, kMaxWorkers> workers_ {};  // last: joined before everything above dies
};
