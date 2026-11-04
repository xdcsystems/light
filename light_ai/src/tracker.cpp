#include "tracker.hpp"

#include <cmath>

namespace {

// Axis-aligned IoU of two boxes (x, y = top-left corner, w/h sizes).
float box_iou( float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh )
{
    const float x0 = ax > bx ? ax : bx;
    const float y0 = ay > by ? ay : by;
    const float x1 = ( ax + aw ) < ( bx + bw ) ? ( ax + aw ) : ( bx + bw );
    const float y1 = ( ay + ah ) < ( by + bh ) ? ( ay + ah ) : ( by + bh );
    const float iw = x1 - x0;
    const float ih = y1 - y0;
    if ( iw <= 0.0f || ih <= 0.0f )
        return 0.0f;
    const float inter = iw * ih;
    const float uni = aw * ah + bw * bh - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

}  // namespace

void Tracker::reset()
{
    tracks_ = {};
    next_id_ = 1;
}

void Tracker::kf_predict( AxisKF& k, float dt ) const
{
    k.x += k.v * dt;
    const float p00 = k.p[0], p01 = k.p[1], p10 = k.p[2], p11 = k.p[3];
    const float dt2 = dt * dt;
    const float q = static_cast<float>( kQAccel );
    k.p[0] = p00 + dt * ( p10 + p01 ) + dt2 * p11 + q * dt2 * dt / 3.0f;
    k.p[1] = p01 + dt * p11 + q * dt2 / 2.0f;
    k.p[2] = p10 + dt * p11 + q * dt2 / 2.0f;
    k.p[3] = p11 + q * dt;
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
    const float p00 = k.p[0], p01 = k.p[1], p10 = k.p[2], p11 = k.p[3];
    k.p[0] = ( 1.0f - k0 ) * p00;
    k.p[1] = ( 1.0f - k0 ) * p01;
    k.p[2] = p10 - k1 * p00;  // original p00 -- the x update above must not leak in
    k.p[3] = p11 - k1 * p01;
}

void Tracker::predict( Track& t, float dt ) const
{
    kf_predict( t.fx, dt );
    kf_predict( t.fy, dt );
}

void Tracker::measure( Track& t, float cx, float cy, float bw, float bh, int64_t ts_us )
{
    kf_update( t.fx, cx );
    kf_update( t.fy, cy );
    const float a = static_cast<float>( kSizeEma ) / 100.0f;
    t.bw += ( bw - t.bw ) * a;
    t.bh += ( bh - t.bh ) * a;
    // Trajectory keeps FACTS only: measured points, never predicted ones.
    t.traj.push( ts_us, cx, cy );
}

void Tracker::spawn( float cx, float cy, float bw, float bh, int64_t ts_us )
{
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
        t.fx.p = { static_cast<float>( kRMeas ), 0.0f, 0.0f, 10000.0f };
        t.fy.p = { static_cast<float>( kRMeas ), 0.0f, 0.0f, 10000.0f };
        t.bw = bw;
        t.bh = bh;
        t.ts_last_us = ts_us;
        t.hits = 1;
        t.miss = 0;
        t.traj.head = 0;
        t.traj.n = 0;
        t.traj.push( ts_us, cx, cy );
        return;
    }
    // No free slot: the detection is dropped (the cap is a safety net,
    // same policy as PodList::push overflow).
}

void Tracker::update( const DetList& dets, int64_t ts_us, TrajList& out )
{
    out.clear();

    // Predict every alive track to the current timestamp. dt comes from
    // the real frame timestamps (VFR-honest) and is clamped both ways:
    // zero/negative (reused slot) must not collapse the filter, a long
    // pause must not explode the prediction.
    for ( Track& t : tracks_ )
    {
        if ( t.life == TRACK_FREE )
            continue;
        float dt = static_cast<float>( ts_us - t.ts_last_us ) * 1e-6f;
        if ( dt < 1e-3f )
            dt = 1e-3f;
        if ( dt > static_cast<float>( kDtMaxMs ) / 1000.0f )
            dt = static_cast<float>( kDtMaxMs ) / 1000.0f;
        predict( t, dt );
        t.ts_last_us = ts_us;
    }

    // Greedy association: each person detection takes the best unmatched
    // alive track (gate kIoUGate); a track is used at most once per frame.
    std::array<unsigned char, 128> det_taken {};  // DetList capacity
    std::array<unsigned char, kMaxTracks> trk_taken {};
    for ( int k = 0; k < dets.n; ++k )
    {
        const Detection& d = dets.d[k];
        if ( d.cls != 0 )
            continue;
        int best = -1;
        float best_iou = 0.0f;
        for ( int i = 0; i < kMaxTracks; ++i )
        {
            const Track& t = tracks_[i];
            if ( t.life == TRACK_FREE || trk_taken[i] )
                continue;
            const float v = box_iou( d.x, d.y, d.w, d.h, t.fx.x - t.bw * 0.5f, t.fy.x - t.bh * 0.5f, t.bw, t.bh );
            // Size-consistency gate: a partial box (person half out
            // of frame) carries a squeezed w/h -- it must not steal
            // the match from the full-size detection.
            if ( t.bw <= 0.0f || t.bh <= 0.0f )
                continue;
            const float rw = d.w / t.bw * 100.0f;
            const float rh = d.h / t.bh * 100.0f;
            if ( rw < static_cast<float>( kSizeMin ) || rw > static_cast<float>( kSizeMax ) ||
                 rh < static_cast<float>( kSizeMin ) || rh > static_cast<float>( kSizeMax ) )
                continue;
            if ( v > best_iou )
            {
                best_iou = v;
                best = i;
            }
        }
        if ( best >= 0 && best_iou * 100.0f >= static_cast<float>( kIoUGate ) )
        {
            trk_taken[best] = 1;
            det_taken[k] = 1;
            Track& t = tracks_[best];
            measure( t, d.x + d.w * 0.5f, d.y + d.h * 0.5f, d.w, d.h, ts_us );
            ++t.hits;
            t.miss = 0;
            if ( t.life == TRACK_TENT && t.hits >= kConfirmHits )
                t.life = TRACK_CONF;
        }
    }

    // Unmatched person detections spawn tentative tracks (slot space
    // permitting).
    for ( int k = 0; k < dets.n; ++k )
    {
        if ( det_taken[k] || dets.d[k].cls != 0 )
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
        if ( !trk_taken[i] )
        {
            ++t.miss;
            if ( t.life == TRACK_TENT && t.miss >= kMaxTentMiss )
                t.life = TRACK_FREE;
            else if ( t.life == TRACK_CONF && t.miss > kMaxMiss )
                t.life = TRACK_FREE;
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
        s.n_pts = t.traj.n;
        out.push( s );
    }
}
