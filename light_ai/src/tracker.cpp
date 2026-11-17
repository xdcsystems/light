#include <algorithm>

#include "tracker.hpp"
#include "core/box_math.hpp"

void Tracker::reset() noexcept
{
    tracks_ = {};
    next_id_ = 1;
}

void Tracker::update( const DetList& dets, int64_t ts_us, TrajList& out ) noexcept
{
    out.clear();
    predict_all( ts_us );
    evict_trails( ts_us );

    DetTrack det_trk {};  // DetList capacity
    det_trk.fill( -1 );
    TrackTaken trk_taken {};
    associate( dets, ts_us, det_trk, trk_taken );
    spawn_unmatched( dets, det_trk, ts_us );

    // Age unmatched tracks (tentative dies fast, confirmed lives through
    // brief detection gaps), then fill the output list.
    for ( int i = 0; i < kMaxTracks; ++i )
    {
        Track& t = tracks_[i];
        if ( t.life == TRACK_FREE )
            continue;
        if ( !trk_taken[i] )
            age( t, ts_us );
        if ( t.life == TRACK_FREE )
            continue;
        out.push( state_of( t ) );
    }
}

// Predict every alive track to the current timestamp. dt comes from the real frame
// timestamps (VFR-honest) and is clamped both ways: zero/negative (reused slot) must not
// collapse the filter, a long pause must not explode the prediction.
void Tracker::predict_all( int64_t ts_us )
{
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
}

void Tracker::predict( Track& t, float dt ) const
{
    // miss >= 2: freeze the state (the IoU gate must still find the reappearing object
    // near the last seen spot) but keep growing the covariance, so the gain
    // at the reappearance update matches the real length of the gap. Tentative
    // tracks reach miss >= 2 as well (death is time-based, kTentMaxAgeMs), and
    // kMaxMiss caps the growth at 15 frames.
    const float q = static_cast<float>( kQAccel );
    if ( t.miss >= kMaxTentMiss )
    {
        t.fx.predict_cov( dt, q );
        t.fy.predict_cov( dt, q );
        return;
    }
    t.fx.predict( dt, q );
    t.fy.predict( dt, q );
}

// Time-based trail eviction, every frame, every alive track: with the
// TrailWindow step gate the pushes are sparse, so eviction inside
// measure() would stall for the whole detection gap and stale points
// would freeze in the ring.
void Tracker::evict_trails( int64_t ts_us )
{
    const int64_t max_age_us = trail_max_age_us();
    for ( Track& t : tracks_ )
    {
        if ( t.life == TRACK_FREE )
            continue;
        t.trail.evict( ts_us - max_age_us );
        // ghost whose trail has fully aged out releases its slot
        if ( t.life == TRACK_DEAD && t.trail.empty() )
            t.life = TRACK_FREE;
    }
}

// Two association rounds (core/associator.hpp): round 1 at the strong gates,
// round 2 at the weak ones for what round 1 left. The track boxes are taken
// afresh before each round; round 2 sees only tracks round 1 did not take.
void Tracker::associate( const DetList& dets, int64_t ts_us, DetTrack& det_trk, TrackTaken& trk_taken )
{
    for ( int round = 0; round < 2; ++round )
    {
        const int nm = assoc_.match( round, dets, track_boxes(), det_trk, trk_taken );
        for ( int p = 0; p < nm; ++p )
        {
            const IouPair& pr = assoc_.pair( p );
            apply_match( tracks_[pr.trk], dets.d[pr.det], round, ts_us );
        }
    }
}

Associator::TrackBoxes Tracker::track_boxes() const
{
    Associator::TrackBoxes boxes {};
    for ( int i = 0; i < kMaxTracks; ++i )
    {
        const Track& t = tracks_[i];
        boxes[i] = TrackBox { t.fx.x(), t.fy.x(), t.bw, t.bh, t.life };
    }
    return boxes;
}

// Round 2 never touches t.hits: a weak match keeps a confirmed track alive but
// cannot promote or confirm a TENT, so weak detector noise stays spawn-only.
void Tracker::apply_match( Track& t, const Detection& d, int round, int64_t ts_us )
{
    // Round-2 dets (weak match: merged blob or squeezed partial box) pass d.h
    // for bottom-anchor offset calculation of cy when clipped, without mutating
    // t.hp, t.bw, or t.bh (trust_size = false).
    measure( t, d.x + d.w * 0.5f, d.y + d.h * 0.5f, d.w, d.h, round == 0, ts_us );
    if ( round == 0 )
    {
        ++t.hits;
        if ( t.life == TRACK_TENT && t.hits >= kConfirmHits )
            t.life = TRACK_CONF;
    }
    t.miss = 0;
    t.ts_last_match_us = ts_us;
}

void Tracker::measure( Track& t, float cx, float cy, float bw, float bh, bool trust_size, int64_t ts_us ) const
{
    if ( trust_size )
        update_height_prior( t, bh );
    const float cy_meas = anchor_clipped( t, cy, bh, trust_size );
    const float r = static_cast<float>( kRMeas );
    const float vmax = static_cast<float>( kVClampPxs );
    t.fx.update( cx, r, vmax );
    t.fy.update( cy_meas, r, vmax );
    if ( trust_size )
    {
        const float a = static_cast<float>( kSizeEma ) / 100.0f;
        t.bw += ( bw - t.bw ) * a;
    }

    // Trail push from the median window of the posteriors (TrailWindow).
    t.trail.feed( ts_us, t.fx.x(), t.fy.x() );

    // Time-based eviction lives in update(), not here: it must run every
    // frame for every alive track, not only on matches -- during a
    // detection gap measure() is never called and stale points would
    // freeze in the ring for the whole gap.
}

// Prior refresh from near-full boxes only. The old condition updated
// the prior from anything >= 0.75*prior: intermediate heights from a
// leaning person passed that test and dragged the prior down, which
// dragged the clip threshold down with it, until anchored boxes
// classified as "full" again and the false crouch returned.
void Tracker::update_height_prior( Track& t, float bh ) const
{
    const float a = static_cast<float>( kSizeEma ) / 100.0f;
    const float full_r = static_cast<float>( kFullBandPct ) / 100.0f;
    if ( t.hp <= 0.0f )
    {
        t.hp = bh;
        t.small_streak = 0;
        t.small_bh = 0.0f;
        return;
    }
    if ( bh >= t.hp * full_r )
    {
        t.hp += ( bh - t.hp ) * a;
        t.small_streak = 0;
        t.small_bh = 0.0f;
        return;
    }
    // Downward adaptation only after kHpDecayStreak mutually consistent smaller
    // boxes, so transient partial occlusions stay anchored while inflated spawn
    // priors or steadily receding people relax cleanly.
    const float tol = static_cast<float>( kHpConsistentPct ) / 100.0f;
    if ( t.small_streak == 0 || bh < t.small_bh * ( 1.0f - tol ) || bh > t.small_bh * ( 1.0f + tol ) )
    {
        t.small_bh = bh;
        t.small_streak = 1;
        return;
    }
    t.small_bh += ( bh - t.small_bh ) * a;
    ++t.small_streak;
    if ( t.small_streak >= kHpDecayStreak )
        t.hp += ( t.small_bh - t.hp ) * ( static_cast<float>( kHpDecayPct ) / 100.0f );
}

// Continuous clip anchor: the anchor strength grows with how clipped
// the box is. The old binary threshold at 0.75*prior made boxes
// hovering near the line flip between "anchored" and "raw" from match
// to match -- a ~100 px center jump per flip (observed velocity
// spikes of -350 px/s). w = (1 - bh/hp) / (1 - clip_r), clamped to
// [0,1]: a full box gets w=0 (raw center), bh <= 0.75*prior gets
// w=1 (fully anchored to bottom + prior height), in between --
// proportional blend, so no jump can exist at the boundary by design.
// Returns the measured center y; updates the bh EMA.
float Tracker::anchor_clipped( Track& t, float cy, float bh, bool update_bh ) const
{
    const float a = static_cast<float>( kSizeEma ) / 100.0f;
    const float clip_r = static_cast<float>( kClipRatioPct ) / 100.0f;
    if ( t.hp <= 0.0f )
    {
        if ( update_bh )
            t.bh += ( bh - t.bh ) * a;
        return cy;
    }
    float w = ( 1.0f - bh / t.hp ) / ( 1.0f - clip_r );
    if ( w < 0.0f )
        w = 0.0f;
    if ( w > 1.0f )
        w = 1.0f;
    // bottom edge stays, lost height restored from the prior
    const float cy_meas = cy + w * ( bh * 0.5f - t.hp * 0.5f );
    // drawn box size: restored from the prior proportionally to w when size-trusted.
    if ( update_bh )
    {
        const float bh_meas = bh + w * ( t.hp - bh );
        t.bh += ( bh_meas - t.bh ) * a;
    }
    return cy_meas;
}

// Unmatched person detections spawn tentative tracks (slot space permitting).
void Tracker::spawn_unmatched( const DetList& dets, const DetTrack& det_trk, int64_t ts_us )
{
    const int nd = std::min( dets.n, static_cast<int>( DetList::cap ) );
    for ( int k = 0; k < nd; ++k )
    {
        if ( det_trk[k] >= 0 || dets.d[k].cls != 0 )
            continue;
        const Detection& d = dets.d[k];
        spawn( d.x + d.w * 0.5f, d.y + d.h * 0.5f, d.w, d.h, ts_us );
    }
}

void Tracker::spawn( float cx, float cy, float bw, float bh, int64_t ts_us )
{
    if ( is_duplicate( cx, cy, bw, bh ) )
    {
        // Silent drop: no console I/O in the frame path (25-50 FPS --
        // synchronous terminal writes cost real latency per frame).
        return;
    }
    for ( Track& t : tracks_ )
    {
        if ( t.life != TRACK_FREE )
            continue;
        init_slot( t, cx, cy, bw, bh, ts_us );
        return;
    }
    // No free slot: the detection is dropped (the cap is a safety net,
    // same policy as PodList::push overflow).
}

// Dedupe: an unmatched detection overlapping an alive track beyond kSpawnDedupeGate
// is treated as a duplicate of it and dropped. The metric is the same box IoU
// the association uses -- symmetric in both sizes, no second notion of "closeness".
// Deliberately no size-consistency check here: a squeezed partial box (person half
// out of frame) fails that gate in association, which is exactly the path that used
// to spawn duplicates -- it must be suppressed, not allowed through.
//
// Only TENT/CONF are real owners of their spot: a DEAD ghost is a
// fading visual trail, its KF state is frozen at the last seen
// position. If a person re-enters the frame while the ghost still
// lingers there, the ghost must NOT suppress the new spawn --
// otherwise the person stays trackless for the whole ghost
// lifetime (kTrailMaxAgeMs).
bool Tracker::is_duplicate( float cx, float cy, float bw, float bh ) const
{
    const float nx = cx - bw * 0.5f;
    const float ny = cy - bh * 0.5f;
    return std::any_of( tracks_.begin(), tracks_.end(), [&]( const Track& t ) {
        if ( ( t.life != TRACK_TENT && t.life != TRACK_CONF ) || t.bw <= 0.0f || t.bh <= 0.0f )
            return false;
        const float iou = box_iou( nx, ny, bw, bh, t.fx.x() - t.bw * 0.5f, t.fy.x() - t.bh * 0.5f, t.bw, t.bh );
        return iou * 100.0f >= static_cast<float>( kSpawnDedupeGate );
    } );
}

void Tracker::init_slot( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us )
{
    t.life = TRACK_TENT;
    t.id = next_id_++;
    t.gen = next_gen_++;
    t.fx.reset( cx, static_cast<float>( kRMeas ), 900.0f );
    t.fy.reset( cy, static_cast<float>( kRMeas ), 900.0f );
    t.bw = bw;
    t.bh = bh;
    // Slot reuse: hp belongs to the previous occupant. A stale hp would anchor
    // a small/crouched successor to the dead track's height: bh >= hp * full_r
    // never fires, so hp never relearns (see measure). The trail restarts
    // with the spawn point (TrailWindow::restart resets the median window).
    t.hp = 0.0f;
    t.small_bh = 0.0f;
    t.small_streak = 0;
    t.trail.restart( ts_us, cx, cy );
    t.ts_last_us = ts_us;
    t.hits = 1;
    t.miss = 0;
    t.ts_last_match_us = ts_us;
}

// An unmatched alive track: one more miss, the velocity decays; a tentative
// track dies after kTentMaxAgeMs without a match, a confirmed one after
// kMaxMiss misses -- into a ghost, not into nothing: the trail keeps
// rendering while its points are alive, fading by age.
void Tracker::age( Track& t, int64_t ts_us ) const
{
    if ( t.life == TRACK_DEAD )
        return;
    ++t.miss;
    const float drop = static_cast<float>( kMissVDrop ) / 100.0f;
    t.fx.damp( drop );
    t.fy.damp( drop );
    const bool tent_expired = t.life == TRACK_TENT &&
        ( ts_us - t.ts_last_match_us ) > static_cast<int64_t>( kTentMaxAgeMs ) * 1000;
    if ( tent_expired )
    {
        t.life = TRACK_FREE;
        t.trail = TrailWindow{};
        return;
    }
    if ( t.life == TRACK_CONF && t.miss > kMaxMiss )
        t.life = TRACK_DEAD;
}

TrackState Tracker::state_of( const Track& t ) const
{
    TrackState s {};
    s.id = t.id;
    s.state = t.life;
    s.cx = t.fx.x();
    s.cy = t.fy.x();
    s.vx = t.fx.v();
    s.vy = t.fy.v();
    s.bw = t.bw;
    s.bh = t.bh;
    s.n_pts = t.trail.size();
    s.gen = t.gen;
    s.hp = t.hp;
    return s;
}

int Tracker::recent_points( int id, int stride, TrajPoint* dst, int dst_cap ) const noexcept
{
    if ( stride <= 0 || dst == nullptr || dst_cap <= 0 )
        return 0;
    for ( const Track& t : tracks_ )
    {
        if ( t.life == TRACK_FREE || t.id != id )
            continue;
        return t.trail.recent( stride, dst, dst_cap );
    }
    return 0;
}
