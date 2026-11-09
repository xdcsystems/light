// Throttled timing accumulator.
#pragma once

#include <chrono>

#include "frame_trace.h"

// Timing accumulator: the throttled avg/60 + TOTAL reports.
// Shared mutable state lives in the owning layer: a Pipeline member,
// not an anonymous-namespace global.
struct Timings
{
    using clk = std::chrono::steady_clock;

    void set_pool( int p )
    {
        if ( p > 1 )
            pool = p;
    }

    void add( const FrameTrace& tr );
    void report( const char* tag );

public:
    double cam = 0, dec = 0, rga = 0, infer = 0, enc = 0;  // sums, ms
    double post = 0.0;
    int n = 0;
    // Pool accounting: in pool mode the per-stage sums double-count
    // the in-flight window overlap (cam wait of frame k vs infer of frame k-2),
    // so sum -> FPS is wrong; FPS comes from the wall clock of the whole run instead.
    // CFR (pool 1) keeps the legacy honest accounting.
    clk::time_point t0{};  // first frame stamp
    long total = 0;        // frames since start
    int pool = 1;          // in-flight frames; 1 = legacy path
};
