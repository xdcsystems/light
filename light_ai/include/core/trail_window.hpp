// TrailWindow: the trajectory of one track -- the ring of pushed trail points
// and the gates in front of it (a median window over the KF posteriors, the
// minimum step). Tracker keeps one per track; the trail dots of the overlay
// come from recent().
//
// Every push is still backed by a real detection (feed() runs only on a match),
// but the pushed position is the KF posterior, not the raw measured center.
// The raw center jumps +-50-100 px when the detector flips between full-body and
// partially-cut boxes: the box SIZE changes, dragging the center even for a still person,
// and no spatial threshold helps against jumps that large. The posterior absorbs the size
// jitter (steady-state gain ~0.1 at r=25) and is also exactly what the box mapper draws as
// the track box, so the trail tracks the rendered box instead of disagreeing with it.
// The kTrajMinStepPx gate still applies: with the posterior as input a standing person
// pushes almost nothing.
//
// Pure logic on the pipeline thread (Tracker::update).

#pragma once

#include <array>
#include <cstdint>

#include "core/track.hpp"  // TrajPoint
#include "pod_ring.hpp"

class TrailWindow
{
public:
    enum
    {
        kTrajPoints = 300,   // ring per track (~12 s at 25 fps)
        kTrajMinStepPx = 8,  // trail push threshold: a new point enters the ring
                             // only when the center moved this far from the last
                             // pushed one; without it detection jitter fills the
                             // ring with a +-few-px cluster around a standing
                             // person, rendering as a swarming/frozen clump
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
    };

    // Ring trajectory: oldest first. PodRing: push() drops the point when the
    // ring is full (it never overwrites), back(k) walks newest -> oldest. The
    // time-based eviction keeps it far below the cap in practice: at most
    // ~120 pushes fit in Tracker::kTrailMaxAgeMs (4 s) at 30 fps.
    using Traj = PodRing<TrajPoint, kTrajPoints>;

    // A fresh trail for a new track: the ring holds the spawn point only,
    // the median window is filled with it and starts counting from zero.
    void restart( int64_t ts_us, float x, float y );

    // One matched KF posterior: into the median window; once the window is
    // full, its median enters the ring if it moved kTrajMinStepPx from the
    // newest point (or the ring is empty).
    void feed( int64_t ts_us, float x, float y );

    // Drops the points older than cutoff_us from the head of the ring. The
    // call that empties a ring holding points (the trail aged out) also resets
    // the median window, once: the trail of a live track restarts from the
    // next kTrailMedianN feeds, with no pre-gap posterior in its first median.
    void evict( int64_t cutoff_us );

    bool empty() const { return ring_.empty(); }
    int size() const { return ring_.size(); }

    // Copies every `stride`-th point into dst, oldest first, newest always
    // included; at most dst_cap points. Returns the count.
    int recent( int stride, TrajPoint* dst, int dst_cap ) const;

private:
    bool far_enough( float mx, float my ) const;

private:
    Traj ring_ {};
    std::array<float, kTrailMedianN> mwin_x_ {};  // median window, see feed()
    std::array<float, kTrailMedianN> mwin_y_ {};
    int mwin_n_ = 0;
};
