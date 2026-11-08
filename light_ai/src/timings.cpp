// timings.cpp (2026-11-08): bodies sliced verbatim from
// the former timings header (13i); set_pool stays inline in the header.

#include "timings.hpp"

#include <iomanip>
#include <iostream>

void Timings::add( const FrameTrace& tr )
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

void Timings::report( const char* tag )
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
