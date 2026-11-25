#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

#include "core/box_filter.hpp"
#include "core/box_math.hpp"
#include "core/detection.hpp"  // Detection, DetList
#include "core/log.hpp"

// Display filtering (BoxFilter: One-Euro + hysteresis dead zone, core/box_filter.hpp) of
// non-person (cls != 0) detection boxes. Persons (cls == 0) pass
// through unsmoothed without consuming track slots, since BoxMapper draws persons
// solely from Tracker while Tracker consumes raw detections directly.
// Global greedy IoU matching: pairs sorted by IoU, best-first;
// class must match. Tuning constants live in the class.
// Confirmed tracks coast across short dropouts (up to kMaxMiss = 6 frames)
// so stationary objects do not blink on 1-6 frame NPU drops; weak single-frame
// detections (< kInstantConf) require kCoastMinHits confirmations to suppress flashes.
// No heap: a fixed std::array of 32 slots, fixed-capacity DetList buffers
// (PodList -- no allocations at all).

class BoxSmoother
{
    struct BoxTrack
    {
        float x = 0, y = 0, w = 0, h = 0;
        float conf = 0.0f;
        BoxFilter filt {};
        int cls = -1;
        int64_t last_seen_us = -1;
        int hits = 0;
        int miss = 0;
        bool alive = false;
        bool matched = false;
    };
    // Shared match-pair type (box_math.hpp); the legacy field order
    // { iou, det, trk } is gone -- aggregate initializers were updated.
    using Pair = IouPair;

    enum
    {
        kMaxMiss = 6,
        kUnconfMaxMiss = 2,
        kMaxMissUs = 300000,        // 300 ms confirmed coast window when ts_us >= 0 (matches 6 * 50 ms)
        kUnconfMaxMissUs = 100000,  // 100 ms unconfirmed window when ts_us >= 0 (matches 2 * 50 ms)
        kCoastMinHits = 2,
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
    void update( const DetList& in, DetList& out, int64_t ts_us = -1 ) noexcept;
    void log_frame( int frame, const DetList& raw, const DetList& smooth );

private:
    float iou( const BoxTrack& t, const Detection& d ) const noexcept;
    int collect_pairs( const DetList& in );
    float step_dt_s( int64_t ts_us ) noexcept;
    void apply_matches( const DetList& in, int na, float dt_s, int64_t ts_us );
    void expire( int64_t ts_us );
    void emit( const DetList& in, const DetTrack& det_trk, DetList& out, int64_t ts_us );
    void emit_coast( DetList& out ) const;
    void spawn( const Detection& d, int64_t ts_us );

private:
    // Named constants, one place to tune.
    const float kMatchThr = 0.3f;
    const float kInstantConf = 0.35f;
    // The filter steps once per update() with a nominal frame period, not the
    // frame time: the --box-log of a replay (layer A, reference/README.md) must not
    // depend on the dequeue timing. 0.05 s ~ the 20-22 FPS of the USB camera.
    const float kFrameDtS = 0.05f;  // nominal fallback when ts_us < 0 (deterministic replay)
    const float kMinDtS = 0.005f;
    const float kMaxDtS = 1.0f;  // > kRawBoxParams.max_dt_s (0.5 s) so long gaps reset BoxFilter
    int64_t last_ts_us_ = -1;

    // Preallocated member: the ~48 KB matching array used to live on
    // update()'s stack, once per call.
    std::array<Pair, kMaxPairs> pairs_ {};
    std::array<BoxTrack, kTrackCount> tracks_ {};
    std::ofstream log_;
};
