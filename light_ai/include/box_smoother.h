#pragma once

#include <array>
#include <fstream>
#include <string>

#include "yolo_postprocess.hpp"

// M3.10: EMA smoothing of detection boxes (owner-approved design).
// Filtering lives in Pipeline; yolo_postprocess stays a pure function.
// Global greedy IoU matching (P1-3: pairs sorted by IoU, assigned
// best-first; class must match); tuning constants live in the class.
// a missed detection keeps the track (and its filtered coords) for up
// to 2 frames so a brief dropout does not reset the filter, more frees
// the slot. No heap: a fixed std::array of 32 slots and fixed-capacity
// DetList buffers (PodList — no allocations at all).
class BoxSmoother
{
    struct BoxTrack
    {
        float x = 0, y = 0, w = 0, h = 0;
        int cls = -1;
        int miss = 0;
        bool alive = false;
        bool matched = false;
    };
    struct Pair
    {
        float iou;
        int det;
        int trk;
    };

    enum
    {
        kMaxMiss = 2,
        kTrackCount = 32,
    // Detections beyond kMaxDet are never matched to existing tracks
    // (they pass through / spawn fresh tracks, as before) 
    // — kMaxDet equals the DetList capacity (128) as a safety net.
        kMaxDet = DetList::cap,
        kMaxPairs = kTrackCount * kMaxDet,
    };

public:
    void set_log( const std::string& path );
    void update( const DetList& in, DetList& out );
    void log_frame( int frame, const DetList& raw, const DetList& smooth );

private:
    float iou( const BoxTrack& t, const Detection& d ) const;

    // Tuning (P1-3): the literals were inline; now named, one place to
    // tune. kAlpha is the EMA weight of the TRACK (0.5 = plain average).
    const float kMatchThr = 0.3f;  // plain member: enum cannot hold float
    const float kAlpha = 0.5f;

    // P1-3 matching buffer: preallocated member (architecture audit
    // item 6: the ~48 KB array lived on update()'s stack per call).
    std::array<Pair, kMaxPairs> pairs_ {};
    std::array<BoxTrack, kTrackCount> tracks_ {};
    std::ofstream log_;
};
