#include <sstream>
#include <string>

#include "check.hpp"
#include "core/log.hpp"
#include "suites.hpp"
#include "timings.hpp"

namespace
{
    int count_of( const std::string& s, const std::string& what )
    {
        int k = 0;
        for ( size_t at = s.find( what ); at != std::string::npos; at = s.find( what, at + what.size() ) )
            ++k;
        return k;
    }

    // Frame i (1-based) costs i ms of cam and nothing else: every window has its own
    // average, so a TOTAL taken from the last window alone shows up as a wrong cam=.
    void feed( Timings& tm, int frames )
    {
        for ( int i = 1; i <= frames; ++i )
        {
            FrameTrace tr;
            tr.cam = i;
            tm.add( tr );
        }
    }

    std::string total_line( const std::string& out )
    {
        const size_t at = out.find( "[timing] TOTAL " );
        return at == std::string::npos ? std::string() : out.substr( at, out.find( '\n', at ) - at );
    }
}  // namespace

void test_timings( TestRun& t )
{
    // 120 frames (a multiple of 60): two avg/60 windows, and TOTAL is still printed,
    // averaged over all 120 frames (cam 1..120 -> 60.5), not over the last window (90.5).
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        Timings tm( log );
        feed( tm, 120 );
        CHECK( t, count_of( out.str(), "[timing] avg/60 " ) == 2 );
        CHECK( t, out.str().find( "[timing] avg/60 cam=30.5 " ) != std::string::npos );
        CHECK( t, out.str().find( "[timing] avg/60 cam=90.5 " ) != std::string::npos );
        tm.report_total();
        CHECK( t, tm.frames() == 120 );
        CHECK( t, total_line( out.str() ) == "[timing] TOTAL  cam=60.5 dec=0.0 rga=0.0 infer=0.0 post=0.0 enc=0.0 | sum=60.5 ms -> ~16.5 FPS" );
        CHECK( t, err.str().empty() );
    }
    // 125 frames: TOTAL covers all 125 (63.0), not the 5-frame tail (123.0).
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        Timings tm( log );
        feed( tm, 125 );
        CHECK( t, count_of( out.str(), "[timing] avg/60 " ) == 2 );
        tm.report_total();
        CHECK( t, tm.frames() == 125 );
        CHECK( t, total_line( out.str() ).find( "[timing] TOTAL  cam=63.0 " ) == 0 );
        // A second TOTAL is the same: the run accumulator is never cleared.
        tm.report_total();
        CHECK( t, count_of( out.str(), "[timing] TOTAL  cam=63.0 " ) == 2 );
    }
    // No frames: no report. Pool mode: wall-clock FPS instead of 1000/sum.
    // The stream's format is restored after the report (Log shares std::cout).
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        Timings tm( log );
        tm.report_total();
        CHECK( t, out.str().empty() );

        tm.set_pool( 3 );
        feed( tm, 5 );
        tm.report_total();
        const std::string line = total_line( out.str() );
        CHECK( t, line.find( "[timing] TOTAL  cam=3.0 " ) == 0 );
        CHECK( t, line.find( "(pool window latency, 3 in flight) | wall ~" ) != std::string::npos );
        out << 1.25;
        CHECK( t, out.str().substr( out.str().size() - 4 ) == "1.25" );
    }
}
