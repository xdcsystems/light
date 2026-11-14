#include <iomanip>
#include <ostream>

#include "timings.hpp"

namespace
{
    // One decimal, fixed. Saves and restores the stream's flags and precision:
    // Log shares std::cout with every other writer, so the format must not leak.
    struct Fixed1
    {
        double v;
    };

    std::ostream& operator<<( std::ostream& os, Fixed1 f )
    {
        const std::ios_base::fmtflags old_flags = os.flags();
        const std::streamsize old_prec = os.precision();
        os << std::fixed << std::setprecision( 1 ) << f.v;
        os.flags( old_flags );
        os.precision( old_prec );
        return os;
    }
}  // namespace

void Timings::add( const FrameTrace& tr )
{
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
}

void Timings::print( const char* tag, const Sums& s ) const
{
    if ( s.n == 0 )
        return;
    const double n = static_cast<double>( s.n );
    const double sum = ( s.cam + s.dec + s.rga + s.infer + s.post + s.enc ) / n;
    // One Log::Line for the whole report: it holds the log mutex, so the line is not interleaved.
    auto line = log_.info();
    line << "[timing] " << tag << " cam=" << Fixed1 { s.cam / n } << " dec=" << Fixed1 { s.dec / n } << " rga=" << Fixed1 { s.rga / n }
         << " infer=" << Fixed1 { s.infer / n } << " post=" << Fixed1 { s.post / n } << " enc=" << Fixed1 { s.enc / n }
         << " | sum=" << Fixed1 { sum };
    if ( pool_ > 1 )
    {
        const double wall = static_cast<double>( run_.n ) / std::chrono::duration<double>( clk::now() - t0_ ).count();
        line << " ms (pool window latency, " << pool_ << " in flight) | wall ~" << Fixed1 { wall } << " FPS\n";
    }
    else
        line << " ms -> ~" << Fixed1 { sum > 0 ? 1000.0 / sum : 0.0 } << " FPS\n";
}
