#include <algorithm>

#include "box_smoother.h"
#include "core/box_math.hpp"

void BoxSmoother::set_log( const std::string& path, const Log& log )
{
    if ( path.empty() )
        return;
    log_.open( path );  // overwrite: each A/B run starts clean
    if ( !log_.is_open() )
        log.warn() << "smoother: box-log open failed for '"
                   << path << "' - A/B logging disabled\n";
}

// The in-order scan let the first detection steal a track just over
// the threshold while a much better match arriving later got
// the leftovers. All pairs are collected, sorted by IoU (descending)
// and assigned best-first - the standard greedy bipartite matching.
// Semantics otherwise unchanged: warm-up pass-through, miss expiry, out order.
void BoxSmoother::update( const DetList& in, DetList& out ) noexcept
{
    out.clear();
    for ( auto& t : tracks_ )
        t.matched = false;

    const int np = collect_pairs( in );
    // Shared greedy matcher (box_math.hpp): same deterministic order
    // (IoU desc, then det, then track) as before. trk_taken mirrors the
    // former tracks_[trk].matched gate: matched is reset at update()
    // start and only set by apply_matches().
    DetTrack det_trk {};
    det_trk.fill( -1 );
    std::array<unsigned char, kTrackCount> trk_taken {};
    const size_t na = greedy_iou_match( pairs_.data(), static_cast<size_t>( np ),
                                        det_trk.data(), trk_taken.data() );
    apply_matches( in, static_cast<int>( na ) );
    expire();
    emit( in, det_trk, out );
}

// Every same-class (det, alive track) pair above kMatchThr into pairs_
// (the preallocated member); returns the count.
int BoxSmoother::collect_pairs( const DetList& in )
{
    int np = 0;
    for ( int i = 0; i < in.n && i < kMaxDet; ++i )
    {
        const Detection& d = in.d[i];
        for ( int t = 0; t < kTrackCount; ++t )
        {
            const BoxTrack& tr = tracks_[t];
            if ( !tr.alive || tr.matched || tr.cls != d.cls )
                continue;
            const float v = iou( tr, d );
            if ( v <= kMatchThr )
                continue;
            if ( np < kMaxPairs )
                pairs_[np++] = { i, t, v };
        }
    }
    return np;
}

// The EMA step of every accepted pair (pairs_[0 .. na), compacted by the matcher).
void BoxSmoother::apply_matches( const DetList& in, int na )
{
    for ( int p = 0; p < na; ++p )
    {
        const Pair& pr = pairs_[p];
        BoxTrack& t = tracks_[pr.trk];
        const Detection& d = in.d[pr.det];
        t.x = kAlpha * t.x + ( 1.0f - kAlpha ) * d.x;
        t.y = kAlpha * t.y + ( 1.0f - kAlpha ) * d.y;
        t.w = kAlpha * t.w + ( 1.0f - kAlpha ) * d.w;
        t.h = kAlpha * t.h + ( 1.0f - kAlpha ) * d.h;
        t.miss = 0;
        t.matched = true;
    }
}

// An alive track without a match this frame: one more miss; past kMaxMiss it frees its slot.
void BoxSmoother::expire()
{
    for ( auto& t : tracks_ )
        if ( t.alive && !t.matched && ++t.miss > kMaxMiss )
            t.alive = false;
}

// The output in input order: a matched det carries its track's smoothed box,
// an unmatched one passes through unsmoothed and spawns a track.
void BoxSmoother::emit( const DetList& in, const DetTrack& det_trk, DetList& out )
{
    for ( int i = 0; i < in.n; ++i )
    {
        const Detection& d = in.d[i];
        Detection sd = d;
        if ( i < kMaxDet && det_trk[i] >= 0 )
        {
            const BoxTrack& t = tracks_[det_trk[i]];
            sd.x = t.x;
            sd.y = t.y;
            sd.w = t.w;
            sd.h = t.h;
        }
        else
        {
            spawn( d );
        }
        out.push( sd );
    }
}

// The first free slot takes the detection: the first sighting passes
// through unsmoothed (warm-up). No free slot: no track.
void BoxSmoother::spawn( const Detection& d )
{
    for ( auto& t : tracks_ )
    {
        if ( t.alive )
            continue;
        t.x = d.x;
        t.y = d.y;
        t.w = d.w;
        t.h = d.h;
        t.cls = d.cls;
        t.miss = 0;
        t.alive = true;
        t.matched = true;
        break;
    }
}

void BoxSmoother::log_frame( int frame, const DetList& raw, const DetList& smooth )
{
    if ( !log_.is_open() )
        return;
    log_ << frame << ',' << raw.n << ',' << smooth.n << '\n';
    for ( int k = 0; k < raw.n; ++k )
    {
        const Detection& d = raw.d[k];
        log_ << "raw," << d << '\n';
    }
    for ( int k = 0; k < smooth.n; ++k )
    {
        const Detection& d = smooth.d[k];
        log_ << "sm," << d << '\n';
    }
}

float BoxSmoother::iou( const BoxTrack& t, const Detection& d ) const noexcept
{
    // Delegates to the shared xywh IoU: BoxTrack and Detection are both
    // top-left + size boxes (box_math.hpp). No semantic change.
    return box_iou( t.x, t.y, t.w, t.h, d.x, d.y, d.w, d.h );
}
