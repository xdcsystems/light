// InferencePool: the NPU stage -- N engines, one InferSlot and one worker
// thread per engine, and a FIFO that hands results back strictly in submit
// order (BoxSmoother and Tracker see frames in order).
//
// Mode, picked once from InferConfig::pool:
//   pool < 3 -> one engine ("ring of one"): the caller submits and collects
//               the same frame, the legacy synchronous path;
//   pool 3   -> three engines, one per NPU core; up to three frames in
//               flight, inference overlapped with capture.
// 2 is rejected by the CLI; anything below 3 maps to one engine here, so a
// direct construction can never create the untested 2-engine mode.
//
// Threading: all public methods run on the pipeline thread -- the FIFO has
// no lock by design. Each worker touches only its own slot and engine (see
// InferSlot for the handoff).
//
// Template parameters (static polymorphism, no virtuals): Engine is
// move-constructible from the EngineArgs of the constructor; Decode is
// copy-constructible, one copy per worker, worker k calls only its own copy,
// so a Decode may keep preallocated per-frame state (YoloDecode: the
// candidate buffer) and is never called concurrently on one instance.
//
// Destruction closes every slot and joins the workers before the engines
// die. A worker blocked inside a wedged rknn_run keeps the destructor
// waiting, exactly as before the move.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <stdexcept>
#include <vector>

#include "core/throw_on.hpp"
#include "config/config.hpp"  // InferConfig
#include "core/frame_view.hpp"  // TensorRef, TensorSet
#include "core/log.hpp"
#include "sys/cpu_affinity.hpp"
#include "npu/infer_slot.hpp"
#include "core/pod_ring.hpp"
#include "core/timings.hpp"  // Timings::clk, ms()
#include "platform/joining_thread.hpp"

struct PoolOptions
{
    bool pin_workers = true;  // pin each worker to the A76 cluster (cpu_affinity.hpp)
    // Bounded wait for one result (P0-2): rknn_run normally always
    // completes; running out of it means a wedged NPU/runtime -- the frame
    // is dropped instead of hanging the pipeline (and Ctrl-C) forever.
    std::chrono::milliseconds result_timeout { 5000 };
};

template <typename Engine, typename Decode>
class InferencePool
{
public:
    enum { kMaxSlots = 3 };

    // Loads the engines (each constructed from engine_args...), sets up
    // their I/O, pins them to NPU cores and starts the workers.
    // Throws std::runtime_error on any failure; nothing is left running then.
    template <typename... EngineArgs>
    InferencePool( const InferConfig& cfg, const Decode& decode, const Log& log, PoolOptions opts,
                   const EngineArgs&... engine_args )
      : decoders_ { { decode, decode, decode } }, log_( log ), opts_( opts ), n_( cfg.pool >= kMaxSlots ? kMaxSlots : 1 )
    {
        engines_.reserve( static_cast<size_t>( n_ ) );  // never reallocates after: &engines_[k] stays valid
        for ( int k = 0; k < n_; ++k )
            engines_.emplace_back( engine_args... );
        for ( auto& e : engines_ )
        {
            throw_on( e.setup_io(), "rknn setup_io" );
        }
        set_core_masks( cfg.core );
        start_workers();  // last: nothing below may throw with workers running
    }

    ~InferencePool() { stop_workers(); }

    InferencePool( const InferencePool& ) = delete;
    InferencePool& operator=( const InferencePool& ) = delete;
    InferencePool( InferencePool&& ) = delete;
    InferencePool& operator=( InferencePool&& ) = delete;

    int size() const { return n_; }
    const Engine& engine( int k ) const { return engines_[static_cast<size_t>( k )]; }

    TensorRef tensor( int k ) const
    {
        const Engine& e = engine( k );
        return { e.input_fd(), e.input_data(), e.input_size(), e.input_stride_bytes() };
    }
    TensorSet tensors() const
    {
        TensorSet set {};
        for ( int k = 0; k < n_; ++k )
            set.push( tensor( k ) );
        return set;
    }

    // --- pipeline thread -------------------------------------------------

    int pending() const { return fifo_.size(); }  // submitted, not collected yet
    bool empty() const { return fifo_.empty(); }
    bool full() const { return fifo_.size() >= n_; }

    // The oldest pending result is ready: collect() will not wait.
    bool front_done() const
    {
        return !fifo_.empty() && slots_[static_cast<size_t>( fifo_.front() )].state() == InferSlot::State::Done;
    }

    // A Free slot whose tensor the caller may fill, or -1. A slot can be
    // missing although the FIFO is not full: an abandoned one stays in
    // flight until its own worker finishes (InferSlot).
    int try_acquire( uint32_t avoid_mask = 0 ) const
    {
        int fallback = -1;
        for ( int i = 0; i < n_; ++i )
        {
            if ( slots_[static_cast<size_t>( i )].state() != InferSlot::State::Free )
                continue;
            if ( ( avoid_mask & ( 1u << static_cast<unsigned>( i ) ) ) == 0 )
                return i;
            if ( fallback < 0 )
                fallback = i;
        }
        return fallback;
    }

    // Queues the frame whose pixels are already in tensor(k). false: the
    // frame is dropped (logged) -- the FIFO is full or slot k is not Free,
    // both caller bugs that the old PubFifo guards reported the same way.
    bool submit( int k, const InferJob& job ) noexcept
    {
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
        fifo_.push( k );
        return true;
    }

    // Takes the oldest pending result, waiting up to PoolOptions::result_timeout.
    // true: `out` holds it and its slot is Free again. false: nothing was
    // pending, or the wait gave up (logged; the slot stays in flight until
    // its worker finishes) -- out.npu_ok is false either way.
    bool collect( InferResult& out ) noexcept
    {
        out.npu_ok = false;
        if ( fifo_.empty() )
        {
            log_.warn() << "pubfifo: empty (n=0), front invalid\n";
            return false;
        }
        const int k = fifo_.front();
        fifo_.pop();
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
    // order. Completed slots are freed; frames still on the NPU are marked
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
    // Pool of one keeps cfg.core verbatim (0 = AUTO: no call -- the A/B
    // baseline is untouched); the 3-pool pins one engine per core.
    void set_core_masks( int core )
    {
        if ( n_ == 1 )
        {
            if ( core != 0 )
            {
                throw_on( engines_[0].set_core_mask( static_cast<unsigned>( core ) ), "rknn set_core_mask" );
                log_.info() << "npu: core mask 0x" << std::hex << core << std::dec << "\n";
            }
            return;
        }
        constexpr std::array<unsigned, kMaxSlots> core_masks { 1U, 2U, 4U };  // NPU core 0 / 1 / 2
        for ( int k = 0; k < n_; ++k )
        {
            throw_on( engines_[static_cast<size_t>( k )].set_core_mask( core_masks[static_cast<size_t>( k )] ), "rknn set_core_mask" );
        }
        log_.info() << "npu: pool of " << n_ << " engines, one per core\n";
    }

    void start_workers()
    {
        try
        {
            for ( int k = 0; k < n_; ++k )
                workers_[static_cast<size_t>( k )].start( &InferencePool::worker_loop, this, k );
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
        for ( auto& slot : slots_ )
            slot.close();
        for ( auto& w : workers_ )
            w.join();
    }

    void worker_loop( int k ) noexcept
    {
        if ( opts_.pin_workers )
            pin_current_thread_rt( "npu worker", 5 + ( k % 3 ), 70, log_ );
        const std::size_t i = static_cast<std::size_t>( k );
        const Engine& eng = engines_[i];
        Decode& dec = decoders_[i];
        InferSlot& slot = slots_[i];
        const auto work = [this, &eng, &dec]( const InferJob& job, InferOutput& out ) { run_job( eng, dec, job, out ); };
        slot.run( work );
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
    static_assert( kMaxSlots == 3, "decoders_ is initialized with one copy per slot" );
    std::array<Decode, kMaxSlots> decoders_;  // one per worker: worker k touches only decoders_[k]
    const Log& log_;
    const PoolOptions opts_;
    const int n_;
    std::vector<Engine> engines_;
    std::array<InferSlot, kMaxSlots> slots_ {};
    PodRing<int, kMaxSlots> fifo_ {};  // slot indices in submit order; pipeline thread only
    std::array<JoiningThread, kMaxSlots> workers_ {};  // last: joined before everything above dies
};
