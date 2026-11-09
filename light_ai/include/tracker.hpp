#pragma once

#include <array>
#include <cstdint>

#include "pod_list.hpp"
#include "yolo_postprocess.hpp"

// ---- Public vocabulary of the tracker -------------------------------
// Data contract shared with consumers, at file scope (as TrackState already was).
// The class below is behavior only; internal structs
// name TrajPoint/TrackLife freely -- file scope precedes the class.

// Track lifecycle states (TrackState.state; consumers filter by it).
enum TrackLife
{
    TRACK_FREE = 0,  // slot unused (internal)
    TRACK_TENT = 1,  // tentative: seen < kConfirmHits times
    TRACK_CONF = 2   // confirmed
};

// One trajectory point (POD, safe to expose).
struct TrajPoint
{
    int64_t ts_us = 0;
    float x = 0.0f, y = 0.0f;
};

// Person trajectory tracker: global greedy IoU association over a per-track
// constant-velocity Kalman filter (one 2-state filter per axis),
// ring trajectory per track.
//
// update() runs once per published frame in publish_oldest()
// (the pipeline thread): it consumes the SMOOTHED detection list
// (BoxSmoother output, scene coordinates) and the frame timestamp.
// VFR is handled honestly -- dt comes from the real timestamps,
// clamped so a pause cannot explode the prediction.
//
// Size-consistency gate: a detection matches a track only if its w/h
// stay within [kSizeMin, kSizeMax] percent of the track's EMA size.
// Partial detections (a person half-out of frame) carry a full-height
// box squeezed to a fraction of the track size -- without the gate
// such a box steals the IoU match and injects a false velocity spike
// into a standing person's track.
//
// out is cleared at entry (preallocated buffer contract) and carries
// ALL alive tracks; tentative tracks are marked TRACK_TENT, the
// consumer filters by state.

struct TrackState
{
    int id;           // stable track id, monotonically growing
    TrackLife state;  // TRACK_TENT / TRACK_CONF
    float cx, cy;     // filtered center (predicted position when unmatched)
    float vx, vy;     // filtered velocity, px/s
    float bw, bh;     // last matched box size (EMA)
    int n_pts;        // trajectory points collected so far
};

using TrajList = PodList<TrackState, 8>;

class Tracker
{
    // Tuning/limits: private x100 integer constants, one place to tune.
    enum
    {
        kMaxTracks = 8,
        kTrajPoints = 300,  // ring per track (~12 s at 25 fps)
        kConfirmHits = 3,   // consecutive matches to confirm a track
        kMaxMiss = 15,      // confirmed track dies after this many misses
        kMaxTentMiss = 2,   // tentative dies after this many misses
        kIoUGate = 30,      // min IoU x100 to accept a match
        kSizeMin = 40,      // det/track size ratio gate, lower x100
        kSizeMax = 200,     // det/track size ratio gate, upper x100
        kDtMaxMs = 500,     // dt clamp, ms
        kQAccel = 2000,     // process noise, px/s^2 (tuning knob)
        kRMeas = 25,        // measurement noise, px^2 (tuning knob)
        kVClampPxs = 500,   // |v| clamp, px/s: a single wild
                            // measurement must not give the filter
                            // an unphysical speed
        kMissVDrop = 90,    // per-miss velocity decay x100 (x0.90
                            // per miss tick): damps coasting so a
                            // missed streak cannot extrapolate the
                            // track across the frame
        kSizeEma = 30,      // box-size EMA alpha x100 (tuning knob)
        kSpawnDedupeGate = kIoUGate / 2  // spawn-dedupe gate, min IoU x100:
                            // an unmatched detection overlapping an alive
                            // track beyond this is a duplicate seen through
                            // a gate failure (size gate, edge IoU jitter)
    };

    // Constant-velocity Kalman, one axis: state (x, v), covariance 2x2
    // flattened (P00, P01, P10, P11).
    struct AxisKF
    {
        float x = 0.0f, v = 0.0f;
        std::array<float, 4> p = { 10000.0f, 0.0f, 0.0f, 10000.0f };
    };

    // Ring trajectory per track: oldest first, newest always included.
    struct Traj
    {
        void push( int64_t ts, float x, float y )
        {
            p[head] = { ts, x, y };
            head = ( head + 1 ) % kTrajPoints;
            if ( n < kTrajPoints )
                ++n;
        }

    public:
        std::array<TrajPoint, kTrajPoints> p {};
        int head = 0;
        int n = 0;
    };

    // One track: two-axis KF + size EMA + lifecycle + ring.
    struct Track
    {
        AxisKF fx, fy;
        float bw = 0.0f, bh = 0.0f;
        int64_t ts_last_us = 0;
        int hits = 0;
        int miss = 0;
        TrackLife life = TRACK_FREE;
        int id = 0;
        Traj traj;
    };

public:
    // Re-init in place: the run loop calls it on restart.
    void reset();

    // Consumes the person detections (cls==0 only, others ignored),
    // predicts, associates, updates the filters and the trajectories,
    // ages unmatched tracks; fills out with all alive tracks.
    void update( const DetList& dets, int64_t ts_us, TrajList& out );

    // Copies every `stride`-th point of track `id`'s ring into
    // dst, oldest first, newest always included; at most dst_cap points.
    // Unknown/free id -> returns 0.
    int recent_points( int id, int stride, TrajPoint* dst, int dst_cap ) const;

private:
    void kf_predict( AxisKF& k, float dt ) const;
    void kf_update( AxisKF& k, float z ) const;
    void predict( Track& t, float dt ) const;
    void measure( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us );
    void associate( const DetList& dets, int64_t ts_us,
                    std::array<unsigned char, 128>& det_taken,
                    std::array<unsigned char, kMaxTracks>& trk_taken );
    void spawn( float cx, float cy, float bw, float bh, int64_t ts_us );

private:
    std::array<Track, kMaxTracks> tracks_ {};
    int next_id_ = 1;
};
