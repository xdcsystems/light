#pragma once

#include <array>
#include <fstream>
#include <string>

#include "core/box_envelope.hpp"
#include "core/box_math.hpp"
#include "core/detection.hpp"  // Detection, DetList
#include "core/log.hpp"

// Interval-envelope + EMA smoothing of non-person (cls != 0) detection boxes. Persons (cls == 0) pass
// through unsmoothed without consuming track slots, since BoxMapper draws persons
// solely from Tracker while Tracker consumes raw detections directly.
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
        BoxEnvelope env {};
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
    using DetTrack = std::array<int, kMaxDet>;  // per det: -1 = unmatched, else the track index

    // Opens the --box-log CSV (empty path: no log); a failure is reported
    // through log and disables the CSV.
    void set_log( const std::string& path, const Log& log );
    void reset() noexcept;
    void update( const DetList& in, DetList& out ) noexcept;
    void log_frame( int frame, const DetList& raw, const DetList& smooth );

private:
    float iou( const BoxTrack& t, const Detection& d ) const noexcept;
    int collect_pairs( const DetList& in );
    void apply_matches( const DetList& in, int na );
    void expire();
    void emit( const DetList& in, const DetTrack& det_trk, DetList& out );
    void spawn( const Detection& d );

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
