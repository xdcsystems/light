#pragma once

#include <array>
#include <cstdint>

#include "core/associator.hpp"    // Associator, TrackBox
#include "core/detection.hpp"     // DetList, Detection
#include "core/kalman_axis.hpp"   // KalmanAxis
#include "core/track.hpp"         // TrackLife, TrajPoint, TrackState, TrajList
#include "core/trail_window.hpp"  // TrailWindow

// Person trajectory tracker: global greedy IoU association (Associator, two
// rounds) over a per-track constant-velocity Kalman filter (KalmanAxis, one
// 2-state filter per axis), ring trajectory per track (TrailWindow).
//
// update() runs once per published frame in publish_oldest() (the pipeline thread):
// it consumes the RAW detection list (YoloDecoder output, scene coordinates;
// BoxSmoother smooths non-person classes only)
// and the frame timestamp. VFR is handled honestly -- dt comes from the real timestamps,
// clamped so a pause cannot explode the prediction.
//
// The association gates (size consistency, IoU, the weak second round) live in
// core/associator.hpp; the trail gates (median window, minimum step) in
// core/trail_window.hpp.
//
// out is cleared at entry (preallocated buffer contract) and carries
// ALL alive tracks; tentative tracks are marked TRACK_TENT, the consumer filters by state.

class Tracker
{
    // Tuning/limits: private x100 integer constants, one place to tune.
    enum
    {
        kMaxTracks = Associator::kMaxTracks,
        kConfirmHits = 3,   // consecutive matches to confirm a track
        // Short coast: 6 misses (~0.24-0.28 s) bridge night detection
        // dropouts up to 6 frames (validated on dataset_night) while clearing
        // exited people well before 0.5 s.
        kMaxMiss = 6,       // confirmed track dies after this many misses
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
        kTrailMaxAgeMs = 4000,  // trail point eviction age: the ring is FIFO by
                                // push count, but with the TrailWindow step gate
                                // pushes became sparse -- during a long standstill
                                // nothing evicts the points accumulated during a
                                // movement episode, and they stay on screen forever
                                // as a frozen clump. Evict by TIME: points older
                                // than this leave from the head of the ring
        kClipRatioPct = 75,  // a measured box height below this fraction of the
                             // height prior is treated as clipped (partial
                             // exit / occlusion): the measured center is
                             // re-anchored to the box bottom plus half prior
                             // height, and the bh EMA is frozen so the drawn
                             // box does not collapse with the detection
        kHpDecayStreak = 8,  // consistent smaller size-trusted matches before hp relaxes down
        kHpConsistentPct = 15,  // +/-15% height stability band for downward hp relaxation
        kHpDecayPct = 10,    // gentle downward EMA step (%) once streak reaches kHpDecayStreak
        kFullBandPct = 90,   // the height prior updates only from boxes at or
                             // above this fraction of the prior. The gap to
                             // kClipRatioPct (75..90) is a hysteresis band:
                             // leaning/partially-visible heights there neither
                             // drag the prior down (which would slowly retune
                             // the clip threshold until anchored boxes read as
                             // "full" and the crouch returns) nor get re-anchored
                             // (they are plausible sizes; the bh EMA follows them)
        kSpawnDedupeGate = Associator::kIoUGate / 2  // spawn-dedupe gate, min IoU x100:
                            // an unmatched detection overlapping an alive
                            // track beyond this is a duplicate seen through
                            // a gate failure (size gate, edge IoU jitter)
    };

    using DetTrack = Associator::DetTrack;
    using TrackTaken = Associator::TrackTaken;

    // One track: two-axis KF + size EMA + lifecycle + trail.
    struct Track
    {
        KalmanAxis fx, fy;
        float bw = 0.0f, bh = 0.0f;
        float hp = 0.0f;  // height prior: quasi-constant person height,
                          // updated only from plausibly full-body boxes,
                          // used to re-anchor clipped ones (see measure)
        float small_bh = 0.0f;  // running height of consistent smaller full boxes
        int small_streak = 0;   // consecutive consistent matches < full band
        int64_t ts_last_us = 0;
        int64_t ts_last_match_us = 0;  // last time measure() matched this
                                       // track: drives the tentative
                                       // time-based patience (kTentMaxAgeMs)
        int hits = 0;
        int miss = 0;
        TrackLife life = TRACK_FREE;
        int id = 0;
        uint32_t gen = 0;
        TrailWindow trail;
    };

public:
    // Trail point lifetime in microseconds, exposed for consumers:
    // the overlay fade uses the same constant as the eviction, so a
    // point's alpha reaches zero exactly when the tracker removes it.
    int64_t trail_max_age_us() const noexcept
    {
        return static_cast<int64_t>( kTrailMaxAgeMs ) * 1000;
    }

    // Re-init in place: the run loop calls it on restart.
    void reset() noexcept;

    // Consumes the person detections (cls==0 only, others ignored),
    // predicts, associates, updates the filters and the trajectories,
    // ages unmatched tracks; fills out with all alive tracks.
    void update( const DetList& dets, int64_t ts_us, TrajList& out ) noexcept;

    // Copies every `stride`-th point of track `id`'s ring into
    // dst, oldest first, newest always included; at most dst_cap points.
    // Unknown/free id -> returns 0.
    int recent_points( int id, int stride, TrajPoint* dst, int dst_cap ) const noexcept;

private:
    // update(), in order:
    void predict_all( int64_t ts_us );
    void evict_trails( int64_t ts_us );
    void associate( const DetList& dets, int64_t ts_us, DetTrack& det_trk, TrackTaken& trk_taken );
    void spawn_unmatched( const DetList& dets, const DetTrack& det_trk, int64_t ts_us );
    void age( Track& t, int64_t ts_us ) const;
    TrackState state_of( const Track& t ) const;

    void predict( Track& t, float dt ) const;
    Associator::TrackBoxes track_boxes() const;
    void apply_match( Track& t, const Detection& d, int round, int64_t ts_us );
    void measure( Track& t, float cx, float cy, float bw, float bh, bool trust_size, int64_t ts_us ) const;
    void update_height_prior( Track& t, float bh ) const;
    float anchor_clipped( Track& t, float cy, float bh, bool update_bh ) const;
    void spawn( float cx, float cy, float bw, float bh, int64_t ts_us );
    bool is_duplicate( float cx, float cy, float bw, float bh ) const;
    void init_slot( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us );

private:
    std::array<Track, kMaxTracks> tracks_ {};
    Associator assoc_ {};
    int next_id_ = 1;       // monotonic across slot reuse (box colors follow)
    uint32_t next_gen_ = 1; // monotonic across track init for async Re-ID validation
};
