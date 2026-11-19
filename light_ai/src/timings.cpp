#include "core/fixed_fmt.hpp"
#include "timings.hpp"

void Timings::add( const FrameTrace& tr, bool npu_ok )
{
    if ( !npu_ok )
    {
        ++npu_fail_;
        return;
    }
    if ( run_.n == 0 )
        t0_ = clk::now();
    run_.add( tr );
    window_.add( tr );
    if ( window_.n == kWindow )
    {
        print( "avg/60", window_ );
        window_ = Sums {};
    }
}

void Timings::report_total() const
{
    print( "TOTAL ", run_ );
    if ( npu_fail_ > 0 )
        log_.info() << "timings: " << npu_fail_ << " frame(s) with a failed NPU job left out of the averages\n";
}

void Timings::print( const char* tag, const Sums& s ) const
{
    if ( s.n == 0 )
        return;
    const double n = static_cast<double>( s.n );
    const double sum = ( s.cam + s.dec + s.rga + s.infer + s.post + s.enc ) / n;
    // One Log::Line for the whole report: it holds the log mutex, so the line is not interleaved.
    auto line = log_.info();
    line << "[timing] " << tag << " cam=" << Fixed { s.cam / n, 1 } << " dec=" << Fixed { s.dec / n, 1 }
         << " rga=" << Fixed { s.rga / n, 1 } << " infer=" << Fixed { s.infer / n, 1 } << " post=" << Fixed { s.post / n, 1 }
         << " enc=" << Fixed { s.enc / n, 1 } << " | sum=" << Fixed { sum, 1 };
    if ( pool_ > 1 )
    {
        // dur > 0 guard: one frame reported right at t0_ (run_.n == 1) divided by zero -> "inf FPS".
        const double dur = std::chrono::duration<double>( clk::now() - t0_ ).count();
        const double wall = dur > 0.0 ? static_cast<double>( run_.n ) / dur : 0.0;
        line << " ms (pool window latency, " << pool_ << " in flight) | wall ~" << Fixed { wall, 1 } << " FPS\n";
    }
    else
        line << " ms -> ~" << Fixed { sum > 0 ? 1000.0 / sum : 0.0, 1 } << " FPS\n";
}
