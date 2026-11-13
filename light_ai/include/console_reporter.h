// Throttled console reporting:
// the every-15th-frame det/track reports from publish_oldest()
// and the final Processed/FPS summary from run().
// Pure output, no state.
// Cadence and the 8-entries cap are fixed contract values.

#pragma once

#include "core/detection.hpp"  // DetList, Detection
#include "core/track.hpp"      // TrajList, TrackState

class ConsoleReporter
{
public:
    void report( int frame, const DetList& dets, const TrajList& trajs, int src_idx = 0, int nsrc = 1 ) const;
    void summary( int ok, double wall, int reconnects ) const;
};

