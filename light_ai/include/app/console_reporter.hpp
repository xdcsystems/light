// Throttled console reporting:
// the every-15th-frame det/track reports from publish_oldest()
// and the final Processed/FPS summary from run().
// Pure output through Log (info, i.e. stdout); no state of its own.
// Each block (the tracks, the boxes, the summary) is one Log::Line, so
// another thread's line cannot land inside it.
// Cadence and the 8-entries cap are fixed contract values.

#pragma once

#include "core/detection.hpp"  // DetList, Detection
#include "core/log.hpp"
#include "core/track.hpp"      // TrajList, TrackState

class ConsoleReporter
{
public:
    explicit ConsoleReporter( const Log& log ) : log_( log ) {}
    void report( int frame, const DetList& dets, const TrajList& trajs, int src_idx = 0, int nsrc = 1 ) const;
    void summary( int ok, double wall, int reconnects ) const;

private:
    void report_tracks( int frame, const TrajList& trajs, int src_idx, int nsrc ) const;
    void report_dets( int frame, const DetList& dets, int src_idx, int nsrc ) const;

private:
    const Log& log_;
};
