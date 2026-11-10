#pragma once

#include <array>
#include <fstream>
#include <string>

#include "box_math.hpp"
#include "yolo_postprocess.hpp"

// EMA smoothing of detection boxes.
// Filtering lives in Pipeline; yolo_postprocess stays a pure function.
// Global greedy IoU matching: pairs sorted by IoU, best-first;
// class must match. Tuning constants live in the class.
// A missed detection keeps the track (and its filtered coords) for up to 2 frames,
// so a brief dropout does not reset the filter; more misses free the slot.
// No heap: a fixed std::array of 32 slots, fixed-capacity DetList buffers
// (PodList -- no allocations at all).

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
    // Shared match-pair type (box_math.hpp); the legacy field order
    // { iou, det, trk } is gone -- aggregate initializers were updated.
    using Pair = IouPair;

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

private:
    // Named constants, one place to tune;
    // kAlpha is the EMA weight of the TRACK (0.5 = plain average).
    const float kMatchThr = 0.3f;
    const float kAlpha = 0.5f;

    // Preallocated member: the ~48 KB matching array used to live on
    // update()'s stack, once per call.
    std::array<Pair, kMaxPairs> pairs_ {};
    std::array<BoxTrack, kTrackCount> tracks_ {};
    std::ofstream log_;
};
