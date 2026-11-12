#pragma once

#include <array>
#include <cstdint>

#include "pod_list.hpp"
#include "pod_ring.hpp"
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

// Person trajectory tracker: global greedy IoU association over a per-track
// constant-velocity Kalman filter (one 2-state filter per axis),
// ring trajectory per track.
//
// update() runs once per published frame in publish_oldest() (the pipeline thread):
// it consumes the SMOOTHED detection list (BoxSmoother output, scene coordinates)
// and the frame timestamp. VFR is handled honestly -- dt comes from the real timestamps,
// clamped so a pause cannot explode the prediction.
//
// Size-consistency gate: a detection matches a track only if its w/h stay 
// within [kSizeMin, kSizeMax] percent of the track's EMA size.
// Partial detections (a person half-out of frame) carry a full-height box squeezed to
// a fraction of the track size -- without the gate such a box steals the IoU 
// match and injects a false velocity spike into a standing person's track.
//
// out is cleared at entry (preallocated buffer contract) and carries
// ALL alive tracks; tentative tracks are marked TRACK_TENT, the consumer filters by state.

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

class Tracker
{
    // Tuning/limits: private x100 integer constants, one place to tune.
    enum
    {
        kMaxTracks = 8,
        kTrajPoints = 300,  // ring per track (~12 s at 25 fps)
        kConfirmHits = 3,   // consecutive matches to confirm a track
        // Short coast: 4 misses (~0.15 s) bridge a 1-2 frame detection
        // dropout without flicker, but a person who left the frame must
        // not keep its box on screen for half a second.
        kMaxMiss = 4,       // confirmed track dies after this many misses
        kMaxTentMiss = 2,   // tentative miss counter: still drives the KF
                            // freeze (miss >= 2) and velocity damping, but
                            // DEATH is time-based now (see kTentMaxAgeMs):
                            // with sparse weak detections the period between
                            // person boxes (4-5 frames) exceeded the
                            // 2-miss patience, so a tentative died before
                            // its second match and the next detection
                            // spawned a fresh track -- a spawn-storm of
                            // one-point ghosts while the person stood in
                            // frame
        kTentMaxAgeMs = 1000,  // tentative dies after this long with no
                               // match: a miss COUNTER is dishonest under
                               // sparse/VFR detection (2 misses can be 70 ms
                               // or 700 ms), wall-clock patience is not.
                               // Covers the observed ~150 ms detection
                               // period with margin; a tent without a
                               // single confirmation never outlives 1 s
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
        kTrajMinStepPx = 8,  // trail push threshold: a new point enters the ring
                             // only when the center moved this far from the last
                             // pushed one; without it detection jitter fills the
                             // ring with a +-few-px cluster around a standing
                             // person, rendering as a swarming/frozen clump
        kTrailMaxAgeMs = 4000,  // trail point eviction age: the ring is FIFO by
                                // push count, but with the kTrajMinStepPx gate
                                // pushes became sparse -- during a long standstill
                                // nothing evicts the points accumulated during a
                                // movement episode, and they stay on screen forever
                                // as a frozen clump. Evict by TIME: points older
                                // than this leave from the head of the ring
        kTrailMedianN = 5,   // trail points are pushed from the median of the
                             // last N KF posteriors: the detector re-sizes
                             // the box every frame and the posterior wobbles
                             // by a few px even so, so the plain step gate
                             // passes physically impossible alternation
                             // (A,B,A,B within 2-3 frames) that renders as
                             // shuffled bright/dim dots along the path. The
                             // median cancels alternation, real drift
                             // survives; lag is N/2 frames, negligible for a
                             // seconds-long trail
        kClipRatioPct = 75,  // a measured box height below this fraction of the
                             // height prior is treated as clipped (partial
                             // exit / occlusion): the measured center is
                             // re-anchored to the box bottom plus half prior
                             // height, and the bh EMA is frozen so the drawn
                             // box does not collapse with the detection
        kFullBandPct = 90,   // the height prior updates only from boxes at or
                             // above this fraction of the prior. The gap to
                             // kClipRatioPct (75..90) is a hysteresis band:
                             // leaning/partially-visible heights there neither
                             // drag the prior down (which would slowly retune
                             // the clip threshold until anchored boxes read as
                             // "full" and the crouch returns) nor get re-anchored
                             // (they are plausible sizes; the bh EMA follows them)
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
    // PodRing: push() wraps at the cap, back(k) walks newest -> oldest.
    using Traj = PodRing<TrajPoint, kTrajPoints>;

    // One track: two-axis KF + size EMA + lifecycle + ring.
    struct Track
    {
        AxisKF fx, fy;
        float bw = 0.0f, bh = 0.0f;
        float hp = 0.0f;  // height prior: quasi-constant person height,
                          // updated only from plausibly full-body boxes,
                          // used to re-anchor clipped ones (see measure)
        float mwin_x[kTrailMedianN] = {};  // median window for trail pushes,
        float mwin_y[kTrailMedianN] = {};  // see measure()
        int mwin_n = 0;
        int64_t ts_last_us = 0;
        int64_t ts_last_match_us = 0;  // last time measure() matched this
                                       // track: drives the tentative
                                       // time-based patience (kTentMaxAgeMs)
        int64_t ts_dead_us = 0;  // moment the track died (TRACK_DEAD), for
                                 // diagnosing; the ghost lifetime is driven by
                                 // the trail points' own ages, not by this
        int hits = 0;
        int miss = 0;
        TrackLife life = TRACK_FREE;
        int id = 0;
        Traj traj;
    };

public:
    // Trail point lifetime in microseconds, exposed for consumers:
    // the overlay fade uses the same constant as the eviction, so a
    // point's alpha reaches zero exactly when the tracker removes it.
    int64_t trail_max_age_us() const
    {
        return static_cast<int64_t>( kTrailMaxAgeMs ) * 1000;
    }

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
    void kf_predict_cov( AxisKF& k, float dt, float q ) const;
    void kf_update( AxisKF& k, float z ) const;
    void predict( Track& t, float dt ) const;
    void measure( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us );
    void associate( const DetList& dets, int64_t ts_us,
                    std::array<int, 128>& det_trk,  // -1 = unmatched, else track idx
                    std::array<unsigned char, kMaxTracks>& trk_taken );
    void spawn( float cx, float cy, float bw, float bh, int64_t ts_us );

private:
    std::array<Track, kMaxTracks> tracks_ {};
    int next_id_ = 1;
};
