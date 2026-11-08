// timings.hpp (2026-11-08): the throttled timing accumulator, moved
// verbatim out of pipeline.hpp (uniformly de-indented one level, no
// code change). Owns the only <iostream>/<iomanip> uses of the
// pipeline interface: the composition-root header no longer pulls the
// report formatting. Shared mutable state lives in the owning layer
// (project rule): a Pipeline member, not an anonymous-namespace
// global.
#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>

#include "frame_trace.hpp"

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

    void add( const FrameTrace& tr )
    {
        if ( total == 0 )
            t0 = clk::now();
        ++total;
        cam += tr.cam;
        dec += tr.dec;
        rga += tr.rga;
        infer += tr.infer;
        post += tr.post;
        enc += tr.enc;
        if ( ++n % 60 == 0 )
            report( "avg/60" );
    }

    void report( const char* tag )
    {
        if ( n == 0 )
            return;
        const double sum = ( cam + dec + rga + infer + post + enc ) / n;
        if ( pool > 1 )
        {
            const double wall = total / std::chrono::duration<double>( clk::now() - t0 ).count();
            std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                      << " rga=" << rga / n << " infer=" << infer / n << " post=" << post / n << " enc=" << enc / n << " | sum=" << sum
                      << " ms (pool window latency, " << pool << " in flight) | wall ~" << wall << " FPS\n";
        }
        else
        {
            std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                      << " rga=" << rga / n << " infer=" << infer / n << " post=" << post / n << " enc=" << enc / n << " | sum=" << sum << " ms -> ~"
                      << ( sum > 0 ? 1000.0 / sum : 0.0 ) << " FPS\n";
        }
        cam = dec = rga = infer = post = enc = 0;
        n = 0;
    }
};
