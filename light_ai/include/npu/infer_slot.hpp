// InferSlot: one handoff cell between the pipeline thread and the NPU
// worker pool.
//
//     Free --submit()--> Queued --finish_work()--> Done --take()--> Free
//                          |                                ^
//                          +--take()/reclaim() give up-->   |
//                             Abandoned --begin/finish------+
//
// A slot whose job is in flight (Queued in the work queue or running on an NPU)
// is freed only by a worker. When the pipeline thread stops waiting
// (take()/reclaim() time out on a wedged NPU), it marks the slot Abandoned, not
// Free: handing the tensor back to RGA while rknn_run still reads it would
// corrupt that inference. A worker frees an Abandoned slot in begin_work()
// (before touching the NPU) or in finish_work() (upon completion).
//
// Payload follows the state, no lock needed:
// - job_: written by submit() (Free, pipeline thread), read by the worker
//   while Queued/Abandoned;
// - out_: written by the worker while Queued/Abandoned, read by take() once
//   Done (pipeline thread).
// The mutex around each transition publishes both.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "core/detection.hpp"  // DetList
#include "core/frame_trace.hpp"
#include "core/geometry.hpp"   // LbGeom
#include "core/non_copyable.hpp"
#include "platform/pi_mutex.hpp"

// What the pipeline thread hands to a worker with one frame. RGA has written
// the frame into tensor(slot) before submit().
struct InferJob
{
    int src_idx = 0;     // owning source
    int src_w = 0;       // source geometry snapshot: the worker scales boxes with it
    int src_h = 0;
    int frame = -1;      // per-source frame index (logs, box CSV)
    int64_t ts_us = -1;  // dequeue time, us from run() start
    bool rga_ok = false; // false: the tensor holds no valid frame, the NPU is skipped
    LbGeom lb {};        // letterbox band of this frame in the tensor
    FrameTrace tr {};    // cam/dec/rga filled at dispatch
};

// What a worker produces for one job.
struct InferOutput
{
    DetList dets {};       // scene-coordinate detections (valid when npu_ok)
    bool npu_ok = false;   // rknn_run and the postprocess both succeeded
    double infer_ms = 0.0; // rknn_run wall time; 0 when the NPU was skipped
    double post_ms = 0.0;  // postprocess wall time; 0 when it did not run
};

// One finished frame as the pipeline thread receives it from the pool.
struct InferResult
{
    InferJob job {};       // as submitted; job.tr.infer/post carry the worker's times
    DetList dets {};
    bool npu_ok = false;
    int slot = -1;         // the tensor slot that ran the job
};

class InferSlot : private NonMovable<InferSlot>
{
public:
    enum class State : uint8_t
    {
        Free,
        Queued,
        Done,
        Abandoned  // still in flight, nobody waits for it: a worker frees it
    };

    InferSlot() = default;
    ~InferSlot() = default;

    void set_free_signal( light_ai::platform::PiMutex* mx, light_ai::platform::PiCondVar* cv ) noexcept
    {
        free_mx_ = mx;
        free_cv_ = cv;
    }

    State state() const noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        return st_;
    }

    bool is_closed() const noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        return closed_;
    }

    State state_and_src( int& out_src ) const noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        out_src = job_.src_idx;
        return st_;
    }

    // --- pipeline thread -------------------------------------------------

    // Source of the job last submitted here (discard() picks frames by it).
    int src_idx() const noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        return job_.src_idx;
    }

    // Free -> Queued. false (nothing changes) if the slot is not Free or is closed.
    bool submit( const InferJob& job ) noexcept
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( st_ != State::Free || closed_ )
                return false;
            job_ = job;
            st_ = State::Queued;
        }
        cv_.notify_all();
        return true;
    }

    // Waits up to `timeout` for the job to finish.
    // Done: copies the result into `out`, the slot becomes Free, returns Done.
    // Otherwise (timeout or close): a Queued slot becomes Abandoned, `out.job`
    // still names the frame for the caller's log, and the state seen at the
    // give-up (Queued) is returned.
    State take( std::chrono::milliseconds timeout, InferResult& out ) noexcept
    {
        State seen = State::Free;
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
            wait_settled( lk, timeout );
            out.job = job_;
            seen = st_;
            if ( st_ != State::Done )
            {
                abandon_if_queued();
                return seen;
            }
            out.job.tr.infer = out_.infer_ms;
            out.job.tr.post = out_.post_ms;
            out.dets = out_.dets;
            out.npu_ok = out_.npu_ok;
            st_ = State::Free;
        }
        notify_free();
        return seen;
    }

    // Discards the job without reading its result: waits up to `timeout`,
    // frees a Done slot (true), abandons a still-Queued one (false).
    bool reclaim( std::chrono::milliseconds timeout ) noexcept
    {
        {
            std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
            wait_settled( lk, timeout );
            if ( st_ != State::Done )
            {
                abandon_if_queued();
                return false;
            }
            st_ = State::Free;
        }
        notify_free();
        return true;
    }

    // Shutdown: releases every wait on this slot; run() returns.
    void close() noexcept
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            closed_ = true;
        }
        cv_.notify_all();
        notify_free();
    }

    // --- worker thread (shared work queue or dedicated slot loop) --------

    // Claims a queued job for execution. Returns true if Queued (job copied
    // to out_job; state remains Queued during execution). If Abandoned, frees
    // the slot immediately and returns false so the NPU skips it.
    bool begin_work( InferJob& out_job ) noexcept
    {
        bool freed = false;
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( closed_ )
                return false;
            if ( st_ == State::Queued )
            {
                out_job = job_;
                return true;
            }
            if ( st_ == State::Abandoned )
            {
                st_ = State::Free;
                freed = true;
            }
        }
        if ( freed )
        {
            cv_.notify_all();
            notify_free();
        }
        return false;
    }

    // Publishes the worker result: Queued -> Done, or Abandoned -> Free.
    void finish_work( const InferOutput& out ) noexcept
    {
        bool freed = false;
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( st_ == State::Abandoned )
            {
                st_ = State::Free;
                freed = true;
            }
            else
            {
                out_ = out;
                st_ = State::Done;
            }
        }
        cv_.notify_all();
        if ( freed )
            notify_free();
    }

    // Dedicated single-slot worker loop (used by unit tests): blocks until
    // a job is queued, runs work( const InferJob&, InferOutput& ) WITHOUT the
    // lock, then publishes the result. Returns once close() is called.
    template <typename Work>
    void run( Work&& work )
    {
        std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
        while ( wait_for_job( lk ) )
            do_job( work, lk );
    }

private:
    void notify_free() const noexcept
    {
        if ( free_mx_ != nullptr && free_cv_ != nullptr )
        {
            {
                const std::lock_guard<light_ai::platform::PiMutex> lk( *free_mx_ );
            }
            free_cv_->notify_all();
        }
    }

    // Returns once the job left Queued, the slot is closed, or the timeout passed.
    void wait_settled( std::unique_lock<light_ai::platform::PiMutex>& lk, std::chrono::milliseconds timeout ) noexcept
    {
        cv_.wait_for( lk, timeout, [this] { return closed_ || st_ != State::Queued; } );
    }

    // Caller holds the lock. Only a worker may free a slot that is on the NPU.
    void abandon_if_queued() noexcept
    {
        if ( st_ == State::Queued )
            st_ = State::Abandoned;
    }

    // Worker: blocks until a job is queued. false: the slot is closed (shutdown).
    bool wait_for_job( std::unique_lock<light_ai::platform::PiMutex>& lk ) noexcept
    {
        cv_.wait( lk, [this] { return closed_ || st_ == State::Queued; } );
        return !closed_;
    }

    // Worker, with the lock held: runs the job without the lock (rknn_run may
    // take seconds), then publishes the result under the lock again.
    template <typename Work>
    void do_job( Work& work, std::unique_lock<light_ai::platform::PiMutex>& lk )
    {
        lk.unlock();
        work( static_cast<const InferJob&>( job_ ), out_ );
        lk.lock();
        const bool freed = ( st_ == State::Abandoned );
        st_ = freed ? State::Free : State::Done;
        lk.unlock();
        cv_.notify_all();
        if ( freed )
            notify_free();
        lk.lock();
    }

private:
    mutable light_ai::platform::PiMutex mx_;
    light_ai::platform::PiCondVar cv_;
    light_ai::platform::PiMutex* free_mx_ = nullptr;
    light_ai::platform::PiCondVar* free_cv_ = nullptr;
    State st_ = State::Free;
    bool closed_ = false;
    InferJob job_ {};
    InferOutput out_ {};
};
