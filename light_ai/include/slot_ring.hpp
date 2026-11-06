// Publish FIFO extracted from pipeline.hpp (stage 4): strictly
// single-threaded - dispatch, publish, drain and pool_discard all
// run on the pipeline thread, so no locks by design. Depth 3 ==
// the maximum NPU pool size. Body below is verbatim from pipeline.hpp.

#ifndef SLOT_RING_HPP
#define SLOT_RING_HPP

#include <cstdint>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "frame_trace.hpp"
#include "rknn_engine.hpp"
#include "yolo_postprocess.hpp"

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
            std::cerr << "pubfifo: overflow (n=" << n << ", head=" << head
                      << "), incoming slot " << s << " dropped\n";
            return;
        }
        slots[( head + n ) % 3] = s;
        ++n;
    }
    int front() const
    {
        return slots[head];
    }
    void pop()
    {
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

#endif  // SLOT_RING_HPP
