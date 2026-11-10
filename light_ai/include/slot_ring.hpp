#pragma once

#include <cstdint>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "frame_trace.h"
#include "pod_ring.hpp"
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
    // at most kMaxSlots of them") survives here as loud logs in the
    // wrappers -- PodRing itself is guard-free by design.
    void push( int s )
    {
        if ( !ring_.push( s ) )
        {
            std::cerr << "pubfifo: overflow (n=" << ring_.n << ", head=" << ring_.head
                      << "), incoming slot " << s << " dropped\n";
        }
    }
    int front() const
    {
        if ( ring_.empty() )
        {
            std::cerr << "pubfifo: empty (n=" << ring_.n << "), front invalid\n";
            return 0;
        }
        return ring_.front();
    }
    void pop()
    {
        if ( ring_.empty() )
        {
            std::cerr << "pubfifo: underflow (n=" << ring_.n << "), pop refused\n";
            return;
        }
        ring_.pop();
    }
    bool empty() const { return ring_.empty(); }
    int size() const { return ring_.size(); }

private:
    enum { kMaxSlots = 3 };
    PodRing<int, kMaxSlots> ring_;
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
    int src_idx = 0;                 // owning source (M5); set at dispatch, read at publish
    int src_w = 0;                   // source geometry snapshot: the worker reads these,
    int src_h = 0;                   // never cur_src_ (it has no view of the main loop)
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
