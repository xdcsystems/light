#pragma once

#include <array>
#include <cstdint>

#include "pod_list.hpp"
#include "yolo_postprocess.hpp"

// Person trajectory tracker: greedy IoU association over a per-track
// constant-velocity Kalman filter (one 2-state filter per axis), ring
// trajectory per track. No heap, no statics, no virtuals -- fixed
// capacity members only (project rules).
//
// update() runs once per published frame in publish_oldest() (the
// pipeline thread): it consumes the SMOOTHED detection list
// (BoxSmoother output, scene coordinates) and the frame timestamp.
// VFR is handled honestly -- dt comes from the real timestamps,
// clamped so a pause cannot explode the prediction.
//
// out is cleared at entry (preallocated buffer contract) and carries
// ALL alive tracks; tentative tracks are marked TRACK_TENT, the
// consumer filters by state.

struct TrackState
{
    int id;        // stable track id, monotonically growing
    int state;     // Tracker::TRACK_TENT / TRACK_CONF
    float cx, cy;  // filtered center (predicted position when unmatched)
    float vx, vy;  // filtered velocity, px/s
    float bw, bh;  // last matched box size (EMA)
    int n_pts;     // trajectory points collected so far
};

using TrajList = PodList<TrackState, 8>;

class Tracker
{
public:
    enum
    {
        kMaxTracks = 8,
        kTrajPoints = 300,  // ring per track (~12 s at 25 fps)
        kConfirmHits = 3,   // consecutive matches to confirm a track
        kMaxMiss = 15,      // confirmed track dies after this many misses
        kMaxTentMiss = 2,   // tentative dies after this many misses
        kIoUGate = 30,      // min IoU x100 to accept a match
        kDtMaxMs = 500,     // dt clamp, ms
        kQAccel = 2000,     // process noise, px/s^2 (tuning knob)
        kRMeas = 25,        // measurement noise, px^2 (tuning knob)
        kSizeEma = 30       // box-size EMA alpha x100 (tuning knob)
    };

    enum TrackLife
    {
        TRACK_FREE = 0,
        TRACK_TENT = 1,
        TRACK_CONF = 2
    };

    void reset();
    // Consumes the person detections (cls==0 only, others ignored),
    // predicts, associates, updates the filters and the trajectories,
    // ages unmatched tracks; fills out with all alive tracks.
    void update( const DetList& dets, int64_t ts_us, TrajList& out );

private:
    struct TrajPoint
    {
        int64_t ts_us;
        float x, y;
    };

    struct Traj
    {
        std::array<TrajPoint, kTrajPoints> p {};
        int head = 0;
        int n = 0;

        void push( int64_t ts, float x, float y )
        {
            p[head] = { ts, x, y };
            head = ( head + 1 ) % kTrajPoints;
            if ( n < kTrajPoints )
                ++n;
        }
    };

    // Constant-velocity Kalman, one axis: state (x, v), covariance 2x2
    // flattened (P00, P01, P10, P11).
    struct AxisKF
    {
        float x = 0.0f, v = 0.0f;
        std::array<float, 4> p = { 10000.0f, 0.0f, 0.0f, 10000.0f };
    };

    struct Track
    {
        AxisKF fx, fy;
        float bw = 0.0f, bh = 0.0f;
        int64_t ts_last_us = 0;
        int hits = 0;
        int miss = 0;
        int life = TRACK_FREE;
        int id = 0;
        Traj traj;
    };

    void kf_predict( AxisKF& k, float dt ) const;
    void kf_update( AxisKF& k, float z ) const;
    void predict( Track& t, float dt ) const;
    void measure( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us );
    void spawn( float cx, float cy, float bw, float bh, int64_t ts_us );

private: // data
    std::array<Track, kMaxTracks> tracks_ {};
    int next_id_ = 1;
};
