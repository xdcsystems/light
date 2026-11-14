// InferencePool: the NPU stage of the pipeline -- N engines, one InferSlot
// and one worker thread per engine, and the FIFO that hands results back
// strictly in submit order (BoxSmoother and Tracker see frames in order).
//
// Moved out of Pipeline (refactoring stage 2): the slot ring, the worker
// threads, the publish FIFO and the core pinning used to be eight
// Pipeline methods sharing its members.
//
// Two modes, picked once from InferConfig::pool:
//   pool < 3 -> one engine ("ring of one"): the caller submits and
//               collects the same frame, the legacy synchronous path;
//   pool 3   -> three engines on one model, one per NPU core; up to
//               three frames in flight, inference overlapped with capture.
// (2 is rejected by the CLI; anything below 3 maps to one engine here, so
// a direct construction can never create the untested 2-engine mode.)
//
// Threading: all public methods are called from one thread, the pipeline
// thread -- the FIFO has no lock by design. Each worker touches only its
// own slot and engine (InferSlot documents the handoff).
//
// Template parameters (static polymorphism, no virtuals -- the test suite
// instantiates the pool with a fake engine, see tests/test_inference_pool.cpp):
//   Engine: move-constructible from the EngineArgs given to the constructor;
//     int setup_io();                 // 0 = ok, once, before any run()
//     int set_core_mask( unsigned );  // 0 = ok; 1/2/4 = NPU core 0/1/2
//     int run() const;                // one inference on the input tensor, 0 = ok
//     int input_fd() const; void* input_data() const; uint32_t input_size() const;
//   Decode: copyable, callable concurrently from all workers:
//     void operator()( const Engine&, const InferJob&, DetList& out ) const;
//
// Destruction closes every slot and joins the workers before the engines
// die. A worker blocked inside a wedged rknn_run keeps the destructor
// waiting, exactly as before the move.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "config/config.hpp"  // InferConfig
#include "core/frame_view.hpp"  // TensorRef, TensorSet
#include "core/log.hpp"
#include "cpu_affinity.hpp"
#include "infer_slot.hpp"
#include "pod_ring.hpp"
#include "timings.hpp"  // Timings::clk, ms()

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
    InferencePool( const InferConfig& cfg, Decode decode, const Log& log, PoolOptions opts,
                   const EngineArgs&... engine_args )
      : decode_( std::move( decode ) ), log_( log ), opts_( opts ), n_( cfg.pool >= kMaxSlots ? kMaxSlots : 1 )
    {
        engines_.reserve( static_cast<size_t>( n_ ) );  // never reallocates after: &engines_[k] stays valid
        for ( int k = 0; k < n_; ++k )
            engines_.emplace_back( engine_args... );
        for ( auto& e : engines_ )
        {
            if ( e.setup_io() != 0 )
                throw std::runtime_error( "rknn setup_io failed" );
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
        return { e.input_fd(), e.input_data(), e.input_size() };
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
    int try_acquire() const
    {
        for ( int k = 0; k < n_; ++k )
        {
            if ( slots_[static_cast<size_t>( k )].state() == InferSlot::State::Free )
                return k;
        }
        return -1;
    }

    // Queues the frame whose pixels are already in tensor(k). false: the
    // frame is dropped (logged) -- the FIFO is full or slot k is not Free,
    // both caller bugs that the old PubFifo guards reported the same way.
    bool submit( int k, const InferJob& job )
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
    bool collect( InferResult& out )
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

    // Drops every pending frame of src_idx (its source detached: those
    // frames are never published). Frames of other sources keep their
    // order. A frame still on the NPU after the bounded wait is abandoned,
    // not freed -- its worker frees the slot.
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
            static_cast<void>( slot.reclaim( opts_.result_timeout ) );
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
                if ( engines_[0].set_core_mask( static_cast<unsigned>( core ) ) != 0 )
                    throw std::runtime_error( "rknn set_core_mask failed" );
                log_.info() << "npu: core mask 0x" << std::hex << core << std::dec << "\n";
            }
            return;
        }
        constexpr std::array<unsigned, kMaxSlots> core_masks { 1U, 2U, 4U };  // NPU core 0 / 1 / 2
        for ( int k = 0; k < n_; ++k )
        {
            if ( engines_[static_cast<size_t>( k )].set_core_mask( core_masks[static_cast<size_t>( k )] ) != 0 )
                throw std::runtime_error( "rknn set_core_mask failed" );
        }
        log_.info() << "npu: pool of " << n_ << " engines, one per core\n";
    }

    void start_workers()
    {
        try
        {
            for ( int k = 0; k < n_; ++k )
                workers_[static_cast<size_t>( k )] = std::thread( &InferencePool::worker_loop, this, k );
        }
        catch ( ... )
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
        {
            if ( w.joinable() )
                w.join();
        }
    }

    void worker_loop( int k )
    {
        if ( opts_.pin_workers )
            pin_current_thread_to_big_cores( "npu worker", log_ );
        const Engine& eng = engines_[static_cast<size_t>( k )];
        InferSlot& slot = slots_[static_cast<size_t>( k )];
        while ( slot.serve( [this, &eng]( const InferJob& job, InferOutput& out ) { run_job( eng, job, out ); } ) )
        {
        }
    }

    // One job on the worker thread: inference, then the postprocess.
    void run_job( const Engine& eng, const InferJob& job, InferOutput& out ) const
    {
        out.dets.clear();
        out.npu_ok = false;
        out.infer_ms = 0.0;
        out.post_ms = 0.0;
        if ( !job.rga_ok )
            return;  // the tensor holds no valid frame: the NPU is skipped
        const auto t0 = Timings::clk::now();
        const bool ran = eng.run() == 0;
        out.infer_ms = ms( t0, Timings::clk::now() );
        if ( !ran )
            return;
        const auto t1 = Timings::clk::now();
        decode_( eng, job, out.dets );
        out.post_ms = ms( t1, Timings::clk::now() );
        out.npu_ok = true;
    }

private:
    const Decode decode_;
    const Log& log_;
    const PoolOptions opts_;
    const int n_;
    std::vector<Engine> engines_;
    std::array<InferSlot, kMaxSlots> slots_ {};
    PodRing<int, kMaxSlots> fifo_ {};  // slot indices in submit order; pipeline thread only
    std::array<std::thread, kMaxSlots> workers_ {};  // last: joined before everything above dies
};
