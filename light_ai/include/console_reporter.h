// Throttled console reporting extracted from Pipeline (stage 3):
// the every-15th-frame det/track reports from publish_oldest() and
// the final Processed/FPS summary from run(). Pure output, no state;
// the cadence and the 8-entries cap match the original verbatim.

#pragma once

#include "tracker.hpp"          // TrajList, TrackState (checked by the patch script)
#include "yolo_postprocess.hpp"  // DetList, Detection (checked by the patch script)

class ConsoleReporter
{
public:
    void report( int frame, const DetList& dets, const TrajList& trajs ) const;

    void summary( int ok, double wall, int reconnects ) const;
};

