#include <algorithm>
#include <cmath>

#include "tracker.hpp"
#include "box_math.hpp"

namespace
{
    // Median of 5 values, 7-comparator selection network: the minimal
    // oblivious net (an exhaustive search confirms no 6-comparator median
    // network exists), loop-free, ~7 branches vs ~10-19 for the former
    // insertion sort. Reorders `v` in place; callers pass a disposable copy.
    float median_of_5( float* v )
    {
        const auto order2 = []( float& a, float& b ) {
            if ( b < a )
            {
                const float t = a;
                a = b;
                b = t;
            }
        };
        order2( v[0], v[1] );
        order2( v[2], v[3] );
        order2( v[0], v[2] );
        order2( v[1], v[3] );
        order2( v[1], v[2] );
        order2( v[1], v[4] );
        order2( v[2], v[4] );
        return v[2];
    }
}  // namespace

void Tracker::reset()
{
    tracks_ = {};
    next_id_ = 1;
}

// Covariance-only predict: grows P exactly like the full predict step but leaves the state
// untouched. Used while a confirmed track is unmatched (miss >= 2): the position stays
// frozen so the IoU gate can still re-associate the reappearing object at the last seen spot,
// but the covariance keeps growing -- otherwise P sat at its post-update size for the whole
// gap and the tiny Kalman gain made the track crawl to the new measurement instead of snapping.

void Tracker::kf_predict_cov( AxisKF& k, float dt, float q ) const
{
    const float p00 = k.p[0];
    const float p01 = k.p[1];
    const float p10 = k.p[2];
    const float p11 = k.p[3];
    const float dt2 = dt * dt;
    k.p[0] = p00 + dt * ( p10 + p01 ) + dt2 * p11 + q * dt2 * dt / 3.0f;
    k.p[1] = p01 + dt * p11 + q * dt2 / 2.0f;
    k.p[2] = p10 + dt * p11 + q * dt2 / 2.0f;
    k.p[3] = p11 + q * dt;
}

void Tracker::kf_predict( AxisKF& k, float dt ) const
{
    k.x += k.v * dt;
    kf_predict_cov( k, dt, static_cast<float>( kQAccel ) );
}

void Tracker::kf_update( AxisKF& k, float z ) const
{
    const float r = static_cast<float>( kRMeas );
    const float s = k.p[0] + r;
    const float k0 = k.p[0] / s;
    const float k1 = k.p[2] / s;
    const float y = z - k.x;
    k.x += k0 * y;
    k.v += k1 * y;
    const float vmax = static_cast<float>( kVClampPxs );
    k.v = std::clamp( k.v, -vmax, vmax );
    const float p00 = k.p[0];
    const float p01 = k.p[1];
    const float p10 = k.p[2];
    const float p11 = k.p[3];
    k.p[0] = ( 1.0f - k0 ) * p00;
    k.p[1] = ( 1.0f - k0 ) * p01;
    k.p[2] = p10 - k1 * p00;  // original p00 -- the x update above must not leak in
    k.p[3] = p11 - k1 * p01;
}

void Tracker::predict( Track& t, float dt ) const
{
    // miss >= 2: freeze the state (the IoU gate must still find the reappearing object
    // near the last seen spot) but keep growing the covariance, so the gain 
    // at the reappearance update matches the real length of the gap. Tentative
    // tracks reach miss >= 2 as well (death is time-based, kTentMaxAgeMs), and
    // kMaxMiss caps the growth at 15 frames.
    if ( t.miss >= kMaxTentMiss )
    {
        kf_predict_cov( t.fx, dt, static_cast<float>( kQAccel ) );
        kf_predict_cov( t.fy, dt, static_cast<float>( kQAccel ) );
        return;
    }
    kf_predict( t.fx, dt );
    kf_predict( t.fy, dt );
}

void Tracker::measure( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us )
{
    // Height prior: the detector flips between full-body and partially-cut
    // boxes; the raw box center then jumps by half the height difference
    // (hundreds of px) and the KF reads it as a crouch even for a person who
    // only edged out of view. Re-anchor the center to the box BOTTOM and the
    // prior height instead: the feet stay put when only the upper part is
    // clipped. The prior updates only from plausibly full boxes, so a run of
    // clipped detections never shrinks it. The bh EMA (drawn box size) is
    // frozen while clipped, so the rendered box does not collapse either.
    const float a = static_cast<float>( kSizeEma ) / 100.0f;
    const float clip_r = static_cast<float>( kClipRatioPct ) / 100.0f;
    const float full_r = static_cast<float>( kFullBandPct ) / 100.0f;
    // Prior refresh from near-full boxes only. The old condition updated
    // the prior from anything >= 0.75*prior: intermediate heights from a
    // leaning person passed that test and dragged the prior down, which
    // dragged the clip threshold down with it, until anchored boxes
    // classified as "full" again and the false crouch returned.
    if ( t.hp <= 0.0f )
        t.hp = bh;
    else if ( bh >= t.hp * full_r )
        t.hp += ( bh - t.hp ) * a;
    // Continuous clip anchor: the anchor strength grows with how clipped
    // the box is. The old binary threshold at 0.75*prior made boxes
    // hovering near the line flip between "anchored" and "raw" from match
    // to match -- a ~100 px center jump per flip (observed velocity
    // spikes of -350 px/s). w = (1 - bh/hp) / (1 - clip_r), clamped to
    // [0,1]: a full box gets w=0 (raw center), bh <= 0.75*prior gets
    // w=1 (fully anchored to bottom + prior height), in between --
    // proportional blend, so no jump can exist at the boundary by design.
    float cy_meas = cy;
    if ( t.hp > 0.0f )
    {
        float w = ( 1.0f - bh / t.hp ) / ( 1.0f - clip_r );
        if ( w < 0.0f )
            w = 0.0f;
        if ( w > 1.0f )
            w = 1.0f;
        // bottom edge stays, lost height restored from the prior
        cy_meas = cy + w * ( bh * 0.5f - t.hp * 0.5f );
        // drawn box size: the clipped fraction is restored from the
        // prior proportionally to w. w=1 reproduces the old frozen-EMA
        // behavior (the box keeps the prior height), w<1 blends
        // smoothly instead of switching the EMA on/off.
        const float bh_meas = bh + w * ( t.hp - bh );
        t.bh += ( bh_meas - t.bh ) * a;
    }
    else
    {
        t.bh += ( bh - t.bh ) * a;
    }
    kf_update( t.fx, cx );
    kf_update( t.fy, cy_meas );
    t.bw += ( bw - t.bw ) * a;
    
    // Every push is still backed by a real detection (measure() runs only on a match),
    // but the pushed position is the KF posterior, not the raw measured center.
    // The raw center jumps +-50-100 px when the detector flips between full-body and
    // partially-cut boxes: the box SIZE changes, dragging the center even for a still person,
    // and no spatial threshold helps against jumps that large. The posterior absorbs the size
    // jitter (steady-state gain ~0.1 at r=25) and is also exactly what fill_gpu_boxes draws as
    // the track box, so the trail tracks the rendered box instead of disagreeing with it.
    // The kTrajMinStepPx gate still applies: with the posterior as input a standing person
    // pushes almost nothing.
    
    // Trail push from a median window (kTrailMedianN posteriors). A stale
    // window must not mix with post-gap positions, so it resets whenever
    // the ring is empty (fresh spawn, or fully aged-out trail).
    if ( t.traj.empty() )
        t.mwin_n = 0;
    t.mwin_x[t.mwin_n % kTrailMedianN] = t.fx.x;
    t.mwin_y[t.mwin_n % kTrailMedianN] = t.fy.x;
    ++t.mwin_n;
    // int overflow guard: past the first full window the counter only
    // feeds the modulo index above, so folding it back by one window
    // keeps both the index cycle and the full-window gate bit-exact.
    if ( t.mwin_n >= 2 * kTrailMedianN )
        t.mwin_n = kTrailMedianN;
    if ( t.mwin_n >= kTrailMedianN )
    {
        float xs[kTrailMedianN];
        float ys[kTrailMedianN];
        std::copy( t.mwin_x, t.mwin_x + kTrailMedianN, xs );
        std::copy( t.mwin_y, t.mwin_y + kTrailMedianN, ys );
        const float mx = median_of_5( xs );
        const float my = median_of_5( ys );
        if ( t.traj.empty() )
        {
            t.traj.push( TrajPoint{ ts_us, mx, my } );
        }
        else
        {
            const TrajPoint last = t.traj.back( 0 );  // newest pushed point
            const float dx = mx - last.x;
            const float dy = my - last.y;
            const float step = static_cast<float>( kTrajMinStepPx );
            if ( dx * dx + dy * dy >= step * step )
                t.traj.push( TrajPoint{ ts_us, mx, my } );
        }
    }

    // Time-based eviction lives in update(), not here: it must run every
    // frame for every alive track, not only on matches -- during a
    // detection gap measure() is never called and stale points would
    // freeze in the ring for the whole gap.
}

void Tracker::associate( const DetList& dets, int64_t ts_us,
                         std::array<int, 128>& det_trk,  // -1 = unmatched
                         std::array<unsigned char, kMaxTracks>& trk_taken )
{
    // Two association rounds over one pair buffer (stage 3, ByteTrack-style):
    // round 1 pairs every person det with every alive track at the strong
    // gate kIoUGate; round 2 pairs round-1-unmatched dets with
    // round-1-unmatched CONF tracks at the weak gate kIoUGateWeak. Target
    // case: person #2 emits conf 0.25-0.55 dets that undershoot the strong
    // IoU gate while their track coasts on damped velocity -- the track
    // used to die at kMaxMiss and the person re-spawned under a new id.
    // The lower size gate is shared; the upper one relaxes in round 2
    // (kSizeMaxWeak): a tent spawned from a partial det stays too small to
    // accept the full-size det of the same person at kSizeMax -- observed
    // live (2026-11-12): a sliver-born track rejected every full-size det
    // and starved to death. Round 2 measures the position only, so the
    // relaxed gate cannot poison the track extent. Round 2 never touches
    // t.hits: a weak match keeps a confirmed track alive but cannot promote
    // or confirm a TENT, so weak detector noise stays spawn-only.
    std::array<IouPair, 128 * kMaxTracks> pairs {};
    const int gates[2] = { kIoUGate, kIoUGateWeak };
    const int size_hi[2] = { kSizeMax, kSizeMaxWeak };
    for ( int round = 0; round < 2; ++round )
    {
        int np = 0;
        for ( int k = 0; k < dets.n; ++k )
        {
            const Detection& d = dets.d[k];
            if ( det_trk[k] >= 0 || d.cls != 0 )
                continue;
            for ( int i = 0; i < kMaxTracks; ++i )
            {
                if ( trk_taken[i] )
                    continue;
                const Track& t = tracks_[i];
                // DEAD ghosts are not match candidates: a re-entering person
                // spawns a fresh track instead of re-attaching to a frozen
                // ghost box. TENT is a round-1 candidate only.
                if ( t.life != TRACK_CONF && !( round == 0 && t.life == TRACK_TENT ) )
                    continue;
                if ( t.bw <= 0.0f || t.bh <= 0.0f )
                    continue;
                const float rw = d.w / t.bw * 100.0f;
                const float rh = d.h / t.bh * 100.0f;
                if ( rw < static_cast<float>( kSizeMin ) || rw > static_cast<float>( size_hi[round] ) ||
                     rh < static_cast<float>( kSizeMin ) || rh > static_cast<float>( size_hi[round] ) )
                    continue;
                const float v = box_iou( d.x, d.y, d.w, d.h,
                                         t.fx.x - t.bw * 0.5f, t.fy.x - t.bh * 0.5f, t.bw, t.bh );
                if ( v * 100.0f >= static_cast<float>( gates[round] ) )
                    pairs[np++] = { k, i, v };
            }
        }
        const size_t nm = greedy_iou_match( pairs.data(), static_cast<size_t>( np ),
                                            det_trk.data(), trk_taken.data() );
        for ( size_t p = 0; p < nm; ++p )
        {
            const Detection& d = dets.d[pairs[p].det];
            Track& t = tracks_[pairs[p].trk];
            // A round-2 det is size-untrusted (merged blob or a squeezed
            // partial box): feed the position only, the track keeps its own
            // extent until a round-1 det re-sizes it.
            measure( t, d.x + d.w * 0.5f, d.y + d.h * 0.5f,
                     round == 0 ? d.w : t.bw, round == 0 ? d.h : t.bh, ts_us );
            if ( round == 0 )
            {
                ++t.hits;
                if ( t.life == TRACK_TENT && t.hits >= kConfirmHits )
                    t.life = TRACK_CONF;
            }
            t.miss = 0;
            t.ts_last_match_us = ts_us;
        }
    }
}


void Tracker::spawn( float cx, float cy, float bw, float bh, int64_t ts_us )
{
    // Dedupe: an unmatched detection overlapping an alive track beyond kSpawnDedupeGate
    // is treated as a duplicate of it and dropped. The metric is the same box IoU 
    // the association uses -- symmetric in both sizes, no second notion of "closeness".
    // Deliberately no size-consistency check here: a squeezed partial box (person half
    // out of frame) fails that gate in association, which is exactly the path that used
    // to spawn duplicates -- it must be suppressed, not allowed through.

    const float nx = cx - bw * 0.5f;
    const float ny = cy - bh * 0.5f;

    // Only TENT/CONF are real owners of their spot: a DEAD ghost is a
    // fading visual trail, its KF state is frozen at the last seen
    // position. If a person re-enters the frame while the ghost still
    // lingers there, the ghost must NOT suppress the new spawn --
    // otherwise the person stays trackless for the whole ghost
    // lifetime (kTrailMaxAgeMs).
    const bool dup = std::any_of( tracks_.begin(), tracks_.end(),
                                  [&]( const Track& t ) {
        if ( ( t.life != TRACK_TENT && t.life != TRACK_CONF ) ||
             t.bw <= 0.0f || t.bh <= 0.0f )
            return false;
        const float iou = box_iou( nx, ny, bw, bh,
                                   t.fx.x - t.bw * 0.5f, t.fy.x - t.bh * 0.5f, t.bw, t.bh );
        return iou * 100.0f >= static_cast<float>( kSpawnDedupeGate );
    } );
    if ( dup )
    {
        // Silent drop: no console I/O in the frame path (25-50 FPS --
        // synchronous terminal writes cost real latency per frame).
        return;
    }

    // Optional: a track that has already missed frames may have drifted away
    // from a genuinely new object standing there. Uncomment to relax the gate
    // for such tracks: return false before the IoU check when t.miss > 0.

    for ( Track& t : tracks_ )
    {
        if ( t.life != TRACK_FREE )
            continue;
        t.life = TRACK_TENT;
        t.id = next_id_++;
        t.fx.x = cx;
        t.fx.v = 0.0f;
        t.fy.x = cy;
        t.fy.v = 0.0f;
        t.fx.p = { static_cast<float>( kRMeas ), 0.0f, 0.0f, 900.0f };
        t.fy.p = { static_cast<float>( kRMeas ), 0.0f, 0.0f, 900.0f };
        t.bw = bw;
        t.bh = bh;
        // Slot reuse: hp and the median window belong to the previous occupant.
        // A stale hp would anchor a small/crouched successor to the dead track's
        // height: bh >= hp * full_r never fires, so hp never relearns (see measure);
        // a stale mwin_n would mix old posteriors into the first trail median --
        // the traj.empty() reset in measure() cannot fire for a fresh spawn,
        // spawn already pushed one point into the ring (cx/cy match the
        // KF posterior at spawn: v=0, x=cx).
        t.hp = 0.0f;
        t.mwin_n = 0;
        std::fill_n( t.mwin_x, kTrailMedianN, cx );
        std::fill_n( t.mwin_y, kTrailMedianN, cy );
        t.ts_last_us = ts_us;
        t.hits = 1;
        t.miss = 0;
        t.ts_last_match_us = ts_us;
        t.traj.clear();
        t.traj.push( TrajPoint{ ts_us, cx, cy } );
        return;
    }
    // No free slot: the detection is dropped (the cap is a safety net,
    // same policy as PodList::push overflow).
}

void Tracker::update( const DetList& dets, int64_t ts_us, TrajList& out )
{
    out.clear();

    // Predict every alive track to the current timestamp. dt comes from the real frame
    // timestamps (VFR-honest) and is clamped both ways: zero/negative (reused slot) must not
    // collapse the filter, a long pause must not explode the prediction.

    for ( Track& t : tracks_ )
    {
        if ( t.life == TRACK_FREE || t.life == TRACK_DEAD )
            continue;
        float dt = static_cast<float>( ts_us - t.ts_last_us ) * 1e-6f;
        if ( dt < 1e-3f )
            dt = 1e-3f;
        if ( dt > static_cast<float>( kDtMaxMs ) / 1000.0f )
            dt = static_cast<float>( kDtMaxMs ) / 1000.0f;
        predict( t, dt );
        t.ts_last_us = ts_us;
    }

    // Time-based trail eviction, every frame, every alive track: with the
    // kTrajMinStepPx push gate the pushes are sparse, so eviction inside
    // measure() would stall for the whole detection gap and stale points
    // would freeze in the ring.
    {
        const int64_t max_age_us = static_cast<int64_t>( kTrailMaxAgeMs ) * 1000;
        for ( Track& t : tracks_ )
        {
            if ( t.life == TRACK_FREE )
                continue;
            while ( !t.traj.empty() && t.traj.front().ts_us < ts_us - max_age_us )
                t.traj.pop();
            // ghost whose trail has fully aged out releases its slot
            if ( t.life == TRACK_DEAD && t.traj.empty() )
                t.life = TRACK_FREE;
        }
    }

    std::array<int, 128> det_trk {};  // DetList capacity
    det_trk.fill( -1 );
    std::array<unsigned char, kMaxTracks> trk_taken {};
    associate( dets, ts_us, det_trk, trk_taken );

    // Unmatched person detections spawn tentative tracks (slot space permitting).
    for ( int k = 0; k < dets.n; ++k )
    {
        if ( det_trk[k] >= 0 || dets.d[k].cls != 0 )
            continue;
        const Detection& d = dets.d[k];
        spawn( d.x + d.w * 0.5f, d.y + d.h * 0.5f, d.w, d.h, ts_us );
    }

    // Age unmatched tracks (tentative dies fast, confirmed lives through
    // brief detection gaps), then fill the output list.
    for ( int i = 0; i < kMaxTracks; ++i )
    {
        Track& t = tracks_[i];
        if ( t.life == TRACK_FREE )
            continue;
        if ( !trk_taken[i] && t.life != TRACK_DEAD )
        {
            ++t.miss;
            const float drop =
                static_cast<float>( kMissVDrop ) / 100.0f;
            t.fx.v *= drop;
            t.fy.v *= drop;
            const bool tent_expired = t.life == TRACK_TENT &&
                ( ts_us - t.ts_last_match_us ) >
                    static_cast<int64_t>( kTentMaxAgeMs ) * 1000;
            if ( tent_expired ||
                 ( t.life == TRACK_CONF && t.miss > kMaxMiss ) )
            {
                // die into a ghost, not into nothing: the trail keeps
                // rendering while its points are alive, fading by age
                t.life = TRACK_DEAD;
                t.ts_dead_us = ts_us;
            }
        }
        if ( t.life == TRACK_FREE )
            continue;
        TrackState s {};
        s.id = t.id;
        s.state = t.life;
        s.cx = t.fx.x;
        s.cy = t.fy.x;
        s.vx = t.fx.v;
        s.vy = t.fy.v;
        s.bw = t.bw;
        s.bh = t.bh;
        s.n_pts = t.traj.size();
        out.push( s );
    }
}

// Oldest-first output, newest point always included: walk the ring backwards,
// keep every `stride`-th hit.
int Tracker::recent_points( int id, int stride, TrajPoint* dst, int dst_cap ) const
{
    if ( stride <= 0 || dst == nullptr || dst_cap <= 0 )
        return 0;
    for ( const Track& t : tracks_ )
    {
        if ( t.life == TRACK_FREE || t.id != id )
            continue;
        const Traj& tr = t.traj;
        const int n = tr.size();
        int w = 0;  // write index into dst
        for ( int k = 0; k < n && w < dst_cap; ++k )
        {
            if ( k % stride != 0 )
                continue;
            dst[w] = tr.back( k );  // back(k): newest (k = 0) -> oldest
            ++w;
        }
        std::reverse( dst, dst + w );
        return w;
    }
    return 0;
}
