// Per-stage timing accumulator: the throttled avg/60 report and the end-of-run TOTAL.
#pragma once

#include <chrono>

#include "core/log.hpp"
#include "frame_trace.h"

// Two independent accumulators over the same FrameTrace stream:
//  - window_: the last 60 frames; add() prints "avg/60" and clears it every 60 frames;
//  - run_:    every frame since start; report_total() prints it as "TOTAL" and never clears it.
// Shared mutable state lives in the owning layer (a Pipeline member); output goes
// through the injected Log (info level). Single-threaded: Pipeline's frame loop only.
class Timings
{
public:
    using clk = std::chrono::steady_clock;

    explicit Timings( const Log& log ) : log_( log ) {}

    void set_pool( int p )
    {
        if ( p > 1 )
            pool_ = p;
    }

    void add( const FrameTrace& tr );
    // Whole-run average; prints nothing when no frame was added.
    void report_total() const;

    long frames() const { return run_.n; }

private:
    // Per-stage sums (ms) over n frames.
    struct Sums
    {
        void add( const FrameTrace& tr )
        {
            cam += tr.cam;
            dec += tr.dec;
            rga += tr.rga;
            infer += tr.infer;
            post += tr.post;
            enc += tr.enc;
            ++n;
        }

    public:
        double cam = 0.0, dec = 0.0, rga = 0.0, infer = 0.0, post = 0.0, enc = 0.0;
        long n = 0;
    };

    void print( const char* tag, const Sums& s ) const;

private:
    enum { kWindow = 60 };  // frames per avg/60 report

    const Log& log_;
    Sums window_ {};  // cleared after each avg/60 report
    Sums run_ {};     // whole run: TOTAL
    // Pool accounting: in pool mode the per-stage sums double-count
    // the in-flight window overlap (cam wait of frame k vs infer of frame k-2),
    // so sum -> FPS is wrong; FPS comes from the wall clock of the whole run instead.
    // CFR (pool 1) keeps the legacy honest accounting.
    clk::time_point t0_ {};  // first frame stamp
    int pool_ = 1;           // in-flight frames; 1 = legacy path
};

// Milliseconds between two steady-clock stamps; was duplicated
// verbatim in pipeline.cpp and overlay_sink.cpp anonymous namespaces.
inline double ms( Timings::clk::time_point a, Timings::clk::time_point b )
{
    return std::chrono::duration<double, std::milli>( b - a ).count();
}
