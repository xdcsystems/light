// Throttled console reporting extracted from Pipeline (stage 3):
// the every-15th-frame det/track reports from publish_oldest() and
// the final Processed/FPS summary from run(). Pure output, no state;
// the cadence and the 8-entries cap match the original verbatim.

#ifndef CONSOLE_REPORTER_HPP
#define CONSOLE_REPORTER_HPP

#include <iomanip>
#include <iostream>

#include "coco_names.h"
#include "tracker.hpp"          // TrajList, TrackState (checked by the patch script)
#include "yolo_postprocess.hpp"  // DetList, Detection (checked by the patch script)

class ConsoleReporter
{
public:
    void report( int frame, const DetList& dets, const TrajList& trajs )
    {
        // Throttled trajectory report: every 15th frame.
        if ( trajs.n > 0 && frame % 15 == 0 )
        {
            std::cout << "track " << frame << ": " << trajs.n << " track(s)\n";
            for ( int k = 0; k < trajs.n && k < 8; ++k )
            {
                const TrackState& t = trajs.d[k];
                std::cout << "  id=" << t.id << ( t.state == TRACK_CONF ? " conf" : " tent" ) << " c=("
                          << t.cx << ',' << t.cy << ") v=(" << t.vx << ',' << t.vy << ") pts=" << t.n_pts << '\n';
            }
        }

        // Throttled report: every 15th frame, up to 8 boxes per report.
        if ( dets.n > 0 && frame % 15 == 0 )
        {
            std::cout << "frame " << frame << ": " << dets.n << " det(s)\n";
            for ( int k = 0; k < dets.n && k < 8; ++k )
            {
                const Detection& d = dets.d[k];
                std::cout << "  cls=" << kCocoNames[d.cls] << " conf=" << std::fixed << std::setprecision( 2 ) << d.conf << " box=("
                          << d.x << ',' << d.y << ' ' << d.w << 'x' << d.h << ")\n";
            }
        }
    }

    void summary( int ok, double wall, int reconnects )
    {
        if ( reconnects > 0 )
            std::cout << "camera disconnects recovered: " << reconnects << '\n';
        std::cout << "Processed " << ok << " frames in " << std::fixed << std::setprecision( 3 ) << wall << " s -> "
                  << ( wall > 0 ? ok / wall : 0.0 ) << " FPS\n";
    }
};

#endif  // CONSOLE_REPORTER_HPP
