// InferSlot: one cell of the inference pool -- the handoff of one frame
// between the pipeline thread and the NPU worker that owns this slot.
//
// State machine (every transition under the slot mutex):
//
//     Free --submit()--> Queued --serve() done--> Done --take()--> Free
//                          |                                ^
//                          +--take()/reclaim() give up-->   |
//                             Abandoned --serve() done------+
//
// The rule the class exists to enforce: a slot whose job is still on the
// NPU can only be freed by its worker. The pipeline thread may give up
// waiting (take() or reclaim() time out on a wedged NPU), but then it
// marks the slot Abandoned instead of freeing it: handing the tensor back
// to RGA while rknn_run still reads it would corrupt that inference. The
// worker frees an Abandoned slot when its job finishes. Before this class
// the rule lived in comments across three Pipeline methods and a `drop`
// flag set in two of them.
//
// Data ownership follows the state, no lock needed for the payload:
// - job_ is written by submit() (Free, pipeline thread) and read by the
//   worker while Queued/Abandoned;
// - out_ is written by the worker while Queued/Abandoned and read by
//   take() once Done (pipeline thread).
// The mutex acquire/release around each transition publishes them.
//
// SDK-free: plain data, std::mutex, std::condition_variable.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

#include "core/detection.hpp"  // DetList
#include "core/geometry.hpp"   // LbGeom
#include "frame_trace.h"

// What the pipeline thread hands to a worker with one frame. The tensor
// itself is not here: slot k always runs on engine k, and RGA has written
// the frame into that engine's input tensor before submit().
struct InferJob
{
    int src_idx = 0;     // owning source
    int src_w = 0;       // source geometry snapshot: the worker scales boxes with it
    int src_h = 0;
    int frame = -1;      // per-source frame index (logs, box CSV)
    int64_t ts_us = -1;  // dequeue time, us from run() start (VFR pts)
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
    int slot = -1;         // the slot (== engine == tensor) that ran the job
};

class InferSlot
{
public:
    enum class State : uint8_t
    {
        Free,
        Queued,
        Done,
        Abandoned  // still on the NPU, nobody waits for it: the worker frees it
    };

    InferSlot() = default;
    ~InferSlot() = default;
    InferSlot( const InferSlot& ) = delete;
    InferSlot& operator=( const InferSlot& ) = delete;
    InferSlot( InferSlot&& ) = delete;
    InferSlot& operator=( InferSlot&& ) = delete;

    State state() const
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        return st_;
    }

    // --- pipeline thread -------------------------------------------------

    // Source of the job last submitted here (discard() picks frames by it).
    int src_idx() const
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        return job_.src_idx;
    }

    // Free -> Queued. false (nothing changes) if the slot is not Free or is closed.
    bool submit( const InferJob& job )
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
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
    State take( std::chrono::milliseconds timeout, InferResult& out )
    {
        std::unique_lock<std::mutex> lk( mx_ );
        wait_settled( lk, timeout );
        out.job = job_;
        const State seen = st_;
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
        return seen;
    }

    // Discards the job without reading its result: waits up to `timeout`,
    // frees a Done slot (true), abandons a still-Queued one (false).
    bool reclaim( std::chrono::milliseconds timeout )
    {
        std::unique_lock<std::mutex> lk( mx_ );
        wait_settled( lk, timeout );
        if ( st_ == State::Done )
        {
            st_ = State::Free;
            return true;
        }
        abandon_if_queued();
        return false;
    }

    // Shutdown: releases every wait on this slot; serve() returns false.
    void close()
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            closed_ = true;
        }
        cv_.notify_all();
    }

    // --- worker thread (one per slot) ------------------------------------

    // Blocks until a job is queued, runs work( const InferJob&, InferOutput& )
    // WITHOUT the lock (the pipeline thread's scans must not wait on
    // rknn_run), then publishes the result: Queued -> Done, or
    // Abandoned -> Free. Returns false once the slot is closed.
    template <typename Work>
    bool serve( Work&& work )
    {
        {
            std::unique_lock<std::mutex> lk( mx_ );
            cv_.wait( lk, [this] { return closed_ || st_ == State::Queued; } );
            if ( closed_ )
                return false;
        }
        std::forward<Work>( work )( static_cast<const InferJob&>( job_ ), out_ );
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            st_ = st_ == State::Abandoned ? State::Free : State::Done;
        }
        cv_.notify_all();
        return true;
    }

private:
    // Returns once the job left Queued, the slot is closed, or the timeout passed.
    void wait_settled( std::unique_lock<std::mutex>& lk, std::chrono::milliseconds timeout )
    {
        cv_.wait_for( lk, timeout, [this] { return closed_ || st_ != State::Queued; } );
    }

    // Caller holds the lock. Only the worker may free a slot that is on the NPU.
    void abandon_if_queued()
    {
        if ( st_ == State::Queued )
            st_ = State::Abandoned;
    }

    mutable std::mutex mx_;
    std::condition_variable cv_;
    State st_ = State::Free;
    bool closed_ = false;
    InferJob job_ {};
    InferOutput out_ {};
};
