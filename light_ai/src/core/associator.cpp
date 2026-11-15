#include "core/associator.hpp"

int Associator::match( int round, const DetList& dets, const TrackBoxes& tracks, DetTrack& det_trk, TrackTaken& trk_taken )
{
    int np = 0;
    for ( int k = 0; k < dets.n; ++k )
    {
        const Detection& d = dets.d[k];
        if ( det_trk[k] >= 0 || d.cls != 0 )
            continue;
        for ( int i = 0; i < kMaxTracks; ++i )
        {
            if ( trk_taken[i] || !candidate( round, tracks[i] ) )
                continue;
            float v = 0.0f;
            if ( gate( round, d, tracks[i], v ) )
                pairs_[np++] = { k, i, v };
        }
    }
    return static_cast<int>( greedy_iou_match( pairs_.data(), static_cast<size_t>( np ), det_trk.data(), trk_taken.data() ) );
}

// DEAD ghosts are not match candidates: a re-entering person spawns a fresh
// track instead of re-attaching to a frozen ghost box. TENT is a round-1
// candidate only (round == 0). A track without an extent cannot be gated.
bool Associator::candidate( int round, const TrackBox& t ) const
{
    if ( t.life != TRACK_CONF && !( round == 0 && t.life == TRACK_TENT ) )
        return false;
    return t.bw > 0.0f && t.bh > 0.0f;
}

// The size gate of the round, then the IoU gate; iou is the box IoU of the pair.
bool Associator::gate( int round, const Detection& d, const TrackBox& t, float& iou ) const
{
    const float size_hi = static_cast<float>( round == 0 ? kSizeMax : kSizeMaxWeak );
    const float size_lo = static_cast<float>( kSizeMin );
    const float rw = d.w / t.bw * 100.0f;
    const float rh = d.h / t.bh * 100.0f;
    if ( rw < size_lo || rw > size_hi || rh < size_lo || rh > size_hi )
        return false;
    iou = box_iou( d.x, d.y, d.w, d.h, t.cx - t.bw * 0.5f, t.cy - t.bh * 0.5f, t.bw, t.bh );
    return iou * 100.0f >= static_cast<float>( round == 0 ? kIoUGate : kIoUGateWeak );
}
