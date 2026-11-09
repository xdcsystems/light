#pragma once

#include <cstdint>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "frame_trace.h"
#include "rga_resizer.h"  // LbGeom: a Slot member by value - the full type is required here
#include "yolo_postprocess.hpp"

class RknnEngine;

// Publish FIFO: strictly single-threaded -- dispatch, publish, drain
// and pool_discard all run on the pipeline thread, so no locks by design.
// Depth 3 == the maximum NPU pool size.
struct PubFifo
{
    // The defensive guards are the point of the design: the depth-3
    // invariant ("in the fifo live exactly the slots with st != FREE;
    // at most kMaxSlots of them") lives in comments across three
    // functions -- one refactor away from a silent corruption
    // (push would overwrite slots_[head_] and inflate n_ past front/pop
    // coherence), so violations get a loud log, never silent damage.

    void push( int s )
    {
        if ( n_ >= kMaxSlots )
        {
            std::cerr << "pubfifo: overflow (n=" << n_ << ", head=" << head_ << "), "
                      << "incoming slot " << s << " dropped\n";
            return;
        }
        slots_[( head_ + n_ ) % kMaxSlots] = s;
        ++n_;
    }
    int front() const
    {
        if ( n_ <= 0 )
        {
            std::cerr << "pubfifo: empty (n=" << n_ << ", head=" << head_ << "), front invalid\n";
            return 0;
        }
        return slots_[head_];
    }
    void pop()
    {
        // Underflow guard: a stray pop() must never drive n_ negative --
        // slots_[(head_ + n_) % kMaxSlots] with n_ < 0 is a negative index (UB).
        // Mirrors the overflow guard in push(): loud log, no corruption.
        if ( n_ <= 0 )
        {
            std::cerr << "pubfifo: underflow (n=" << n_ << ", head=" << head_ << "), pop refused\n";
            return;
        }
        head_ = ( head_ + 1 ) % kMaxSlots;
        --n_;
    }
    bool empty() const { return n_ == 0; }
    int size() const { return n_; }

private:
    enum { kMaxSlots = 3 };
    int slots_[kMaxSlots] {};
    int head_ = 0;
    int n_ = 0;
};

// Unlike PubFifo above, Slot is NOT single-threaded: the mutex/cv pair
// guards the handoff between the pipeline thread and the worker.

// Context-pool machinery (active only with --pool 3).
// A slot is a three-state cell: FREE -> QUEUED (main dispatches a
// frame) -> DONE (worker finished infer + postprocess) -> FREE
// (main published). One slot per engine; the ring depth equals the
// pool size, publish-before-reuse keeps tensor ownership safe and
// frame order strictly sequential (BoxSmoother sees the legacy order).
struct Slot
{
    RknnEngine* eng = nullptr;
    DetList dets;                    // worker-side postprocess output
    int frame = -1;                  // global frame index (logs)
    int64_t ts_us = -1;              // dequeue time, µs from start (VFR pts)
    bool rga_ok = false;             // captured at dispatch
    bool npu_ok = false;             // worker verdict, read at publish
    bool drop = false;               // publish gave up: the worker frees the slot itself
    LbGeom lb;                       // letterbox geometry at dispatch (single-owner struct)
    FrameTrace tr;                   // per-frame timings: dispatch->cam/dec/rga, worker->infer, publish->enc
    std::mutex mx;
    std::condition_variable cv;
    enum St : uint8_t { FREE, QUEUED, DONE } st = FREE;
};
