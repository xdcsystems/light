// timings.hpp (2026-11-08): the throttled timing accumulator, moved
// verbatim out of pipeline.hpp (uniformly de-indented one level, no
// code change). (13i: the bodies moved to timings.cpp -- the
// <iostream>/<iomanip> uses moved with them; the header owns none
// and the composition root no longer pulls the report formatting.) Shared mutable state lives in the owning layer
// (project rule): a Pipeline member, not an anonymous-namespace
// global.
#pragma once

#include <chrono>

#include "frame_trace.h"

// Timing accumulator: the throttled avg/60 + TOTAL reports.
// Shared mutable state lives in the owning layer (project rule):
// a Pipeline member, not an anonymous-namespace global.
struct Timings
{
    using clk = std::chrono::steady_clock;

    double cam = 0, dec = 0, rga = 0, infer = 0, enc = 0;  // sums, ms
    double post = 0.0;
    int n = 0;
    // Pool accounting: in pool mode the per-stage sums double-count
    // the in-flight window overlap (cam wait of frame k vs infer of
    // frame k-2), so sum -> FPS is wrong; FPS comes from the wall
    // clock of the whole run instead. CFR (pool 1) keeps the legacy
    // honest accounting.
    clk::time_point t0{};  // first frame stamp
    long total = 0;        // frames since start
    int pool = 1;          // in-flight frames; 1 = legacy path

    void set_pool( int p )
    {
        if ( p > 1 )
            pool = p;
    }

    void add( const FrameTrace& tr );
    void report( const char* tag );
};
