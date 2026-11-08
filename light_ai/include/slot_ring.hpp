// Publish FIFO extracted from pipeline.hpp (stage 4).
// Body below is verbatim from pipeline.hpp; the design contract
// (single-threaded, depth 3) lives in the struct comment below.

#pragma once

#include <cstdint>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "frame_trace.h"
#include "rga_resizer.h"  // LbGeom: a Slot member by value --
                            // the full type is required (was only
                            // supplied by pipeline.hpp order)
#include "yolo_postprocess.hpp"

// RknnEngine appears in Slot only as a pointer (never
// dereferenced in this header) -- forward-declared.
class RknnEngine;

// Publish FIFO: strictly single-threaded — dispatch, publish, drain
// and pool_discard all run on the pipeline thread, so no locks by
// design. Depth 3 == the maximum NPU pool size.
struct PubFifo
{
    int slots[3] = {};
    int head = 0;
    int n = 0;

    void push( int s )
    {
        // Defensive guard: the depth-3 invariant ("in pub_ live exactly
        // the slots with st != FREE; at most npool_ <= 3 of them") is
        // proven by the slot machine, but it lives in comments across
        // three functions. One refactor away from a silent corruption
        // (push would overwrite slots[head] and inflate n past front/pop
        // coherence) -- so the violation gets a loud log instead.
        // Strictly single-threaded by design (see struct comment): the
        // cerr race risk with worker output exists only if this ever
        // stops being true.
        if ( n >= 3 )
        {
            std::cerr << "pubfifo: overflow (n=" << n << ", head=" << head << "), "
                      << "incoming slot " << s << " dropped\n";
            return;
        }
        slots[( head + n ) % 3] = s;
        ++n;
    }
    int front() const
    {
        if ( n <= 0 )
        {
            std::cerr << "pubfifo: empty (n=" << n << ", head=" << head << "), front invalid\n";
            return 0;
        }
        return slots[head];
    }
    void pop()
    {
        // Underflow guard: a stray pop() must never drive n negative --
        // slots[(head + n) % 3] with n < 0 is a negative index (UB).
        // Mirrors the overflow guard in push(): loud log, no corruption.
        if ( n <= 0 )
        {
            std::cerr << "pubfifo: underflow (n=" << n << ", head=" << head << "), pop refused\n";
            return;
        }
        head = ( head + 1 ) % 3;
        --n;
    }
};

// Slot moved here in stage 6 (unblocked by the FrameTrace extraction).
// Unlike PubFifo above, Slot is NOT single-threaded: the mutex/cv pair
// guards the handoff between the pipeline thread and the worker. The
// comment inside is verbatim from pipeline.hpp.

// --- M4: context-pool machinery (active only with --pool 3) ---
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
    LbGeom lb;  // letterbox geometry at dispatch (single-owner struct)
    FrameTrace tr;  // per-frame timings: dispatch->cam/dec/rga, worker->infer, publish->enc
    std::mutex mx;
    std::condition_variable cv;
    enum St : uint8_t { FREE, QUEUED, DONE } st = FREE;
};

