// Track vocabulary: the data contract between the Tracker and its consumers
// (box mapper, console reporter, overlay). No behavior, no SDK dependencies
// (moved out of tracker.hpp; the Tracker class itself stays there).

#pragma once

#include <cstdint>

#include "pod_list.hpp"

// Track lifecycle states (TrackState.state; consumers filter by it).
enum TrackLife
{
    TRACK_FREE = 0,  // slot unused (internal)
    TRACK_TENT = 1,  // tentative: seen < kConfirmHits times
    TRACK_CONF = 2,  // confirmed
    TRACK_DEAD = 3   // dead: not predicted / associated / aged, but its trail
                     // keeps rendering until the points age out (kTrailMaxAgeMs),
                     // then the slot returns to FREE. Without the ghost state a
                     // track that leaves the frame kills its trail instantly --
                     // all its points vanish in one frame instead of fading out.
};

// One trajectory point (POD, safe to expose).
struct TrajPoint
{
    int64_t ts_us = 0;
    float x = 0.0f, y = 0.0f;
};

// Snapshot of one track, filled by Tracker::update() into TrajList.
struct TrackState
{
    int id;           // stable track id, monotonically growing
    TrackLife state;  // TRACK_TENT / TRACK_CONF
    float cx, cy;     // filtered center (predicted position when unmatched)
    float vx, vy;     // filtered velocity, px/s
    float bw, bh;     // last matched box size (EMA)
    int n_pts;        // trajectory points collected so far
                      // state may also be TRACK_DEAD: no box, only
                      // the trail keeps drawing (fading, path history)
};

using TrajList = PodList<TrackState, 8>;
