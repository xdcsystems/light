#include <iomanip>
#include <iostream>

#include "console_reporter.h"
#include "coco_names.h"

namespace
{
    struct CoutFmtGuard
    {
        std::ios_base::fmtflags flags = std::cout.flags();
        std::streamsize prec = std::cout.precision();
        ~CoutFmtGuard()
        {
            std::cout.flags( flags );
            std::cout.precision( prec );
        }
    };
}  // namespace

void ConsoleReporter::report(
    int frame, const DetList& dets, const TrajList& trajs, int src_idx, int nsrc ) const
{
    const CoutFmtGuard fmt_guard {};
    // Throttled trajectory report: every 15th frame.
    if ( trajs.n > 0 && frame % 15 == 0 )
    {
        std::cout << "track " << frame;
        if ( nsrc > 1 )
            std::cout << " s" << src_idx;
        std::cout << ": " << trajs.n << " track(s)\n";
        for ( int k = 0; k < trajs.n && k < 8; ++k )
        {
            const TrackState& t = trajs.d[k];
            const char* life_tag = ( t.state == TRACK_CONF )  ? " conf"
                                   : ( t.state == TRACK_DEAD ) ? " dead"
                                                                : " tent";
            std::cout << "  id=" << t.id << life_tag
                      << std::fixed << std::setprecision( 1 ) << " c=("
                      << t.cx << ',' << t.cy << ") v=(" << t.vx << ',' << t.vy << ") pts=" << t.n_pts << '\n';
        }
    }

    // Throttled report: every 15th frame, up to 8 boxes per report.
    if ( dets.n > 0 && frame % 15 == 0 )
    {
        std::cout << "frame " << frame;
        if ( nsrc > 1 )
            std::cout << " s" << src_idx;
        std::cout << ": " << dets.n << " det(s)\n";
        for ( int k = 0; k < dets.n && k < 8; ++k )
        {
            const Detection& d = dets.d[k];
            const char* cls_name = ( d.cls >= 0 && d.cls < 80 ) ? kCocoNames[d.cls] : "?";
            std::cout << "  cls=" << cls_name << " conf=" << std::fixed << std::setprecision( 2 ) << d.conf << " box=("
                      << d.x << ',' << d.y << ' ' << d.w << 'x' << d.h << ")\n";
        }
    }
}

void ConsoleReporter::summary( int ok, double wall, int reconnects ) const
{
    const CoutFmtGuard fmt_guard {};
    if ( reconnects > 0 )
        std::cout << "camera disconnects recovered: " << reconnects << '\n';
    std::cout << "Processed " << ok << " frames in " << std::fixed << std::setprecision( 3 ) << wall << " s -> "
              << ( wall > 0 ? ok / wall : 0.0 ) << " FPS\n";
}
