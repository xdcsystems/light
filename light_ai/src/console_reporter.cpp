#include "console_reporter.h"
#include "npu/coco_names.hpp"
#include "core/fixed_fmt.hpp"

namespace
{
    enum
    {
        kEvery = 15,  // report cadence, frames
        kMaxEntries = 8  // tracks / boxes per report
    };

    const char* life_tag( TrackLife state )
    {
        if ( state == TRACK_CONF )
            return " conf";
        if ( state == TRACK_DEAD )
            return " dead";
        return " tent";
    }

    const char* cls_name( int cls )
    {
        return ( cls >= 0 && cls < 80 ) ? kCocoNames[cls] : "?";
    }
}  // namespace

void ConsoleReporter::report( int frame, const DetList& dets, const TrajList& trajs, int src_idx, int nsrc ) const
{
    // Throttled: every 15th frame, trajectories first, then up to 8 boxes.
    if ( frame % kEvery != 0 )
        return;
    if ( trajs.n > 0 )
        report_tracks( frame, trajs, src_idx, nsrc );
    if ( dets.n > 0 )
        report_dets( frame, dets, src_idx, nsrc );
}

void ConsoleReporter::report_tracks( int frame, const TrajList& trajs, int src_idx, int nsrc ) const
{
    Log::Line line = log_.info();
    line << "track " << frame;
    if ( nsrc > 1 )
        line << " s" << src_idx;
    line << ": " << trajs.n << " track(s)\n";
    for ( int k = 0; k < trajs.n && k < kMaxEntries; ++k )
    {
        const TrackState& t = trajs.d[k];
        line << "  id=" << t.display_id();
        if ( t.global_id > 0 )
            line << "(L" << t.id << ')';
        line << life_tag( t.state ) << " c=(" << Fixed { t.cx, 1 } << ',' << Fixed { t.cy, 1 } << ") v=("
             << Fixed { t.vx, 1 } << ',' << Fixed { t.vy, 1 } << ") pts=" << t.n_pts << '\n';
    }
}

void ConsoleReporter::report_dets( int frame, const DetList& dets, int src_idx, int nsrc ) const
{
    Log::Line line = log_.info();
    line << "frame " << frame;
    if ( nsrc > 1 )
        line << " s" << src_idx;
    line << ": " << dets.n << " det(s)\n";
    for ( int k = 0; k < dets.n && k < kMaxEntries; ++k )
    {
        const Detection& d = dets.d[k];
        line << "  cls=" << cls_name( d.cls ) << " conf=" << Fixed { d.conf, 2 } << " box=(" << Fixed { d.x, 2 } << ','
             << Fixed { d.y, 2 } << ' ' << Fixed { d.w, 2 } << 'x' << Fixed { d.h, 2 } << ")\n";
    }
}

void ConsoleReporter::summary( int ok, double wall, int reconnects ) const
{
    Log::Line line = log_.info();
    if ( reconnects > 0 )
        line << "camera disconnects recovered: " << reconnects << '\n';
    line << "Processed " << ok << " frames in " << Fixed { wall, 3 } << " s -> " << Fixed { wall > 0 ? ok / wall : 0.0, 3 } << " FPS\n";
}
