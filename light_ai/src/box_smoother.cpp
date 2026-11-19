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
void BoxSmoother::reset() noexcept
{
    tracks_ = {};
}

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
        if ( d.cls == 0 || !det_usable( d ) )
            continue;
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

// The filter step of every accepted pair (pairs_[0 .. na), compacted by the matcher).
void BoxSmoother::apply_matches( const DetList& in, int na )
{
    for ( int p = 0; p < na; ++p )
    {
        const Pair& pr = pairs_[p];
        BoxTrack& t = tracks_[pr.trk];
        const Detection& d = in.d[pr.det];
        t.filt.step( BoxCWH { d.x + 0.5f * d.w, d.y + 0.5f * d.h, d.w, d.h }, kFrameDtS, kFilter );
        const BoxCWH o = t.filt.out();
        t.x = o.cx - 0.5f * o.w;
        t.y = o.cy - 0.5f * o.h;
        t.w = o.w;
        t.h = o.h;
        t.conf = d.conf;
        t.hits = std::min( t.hits + 1, static_cast<int>( kCoastMinHits ) );
        t.miss = 0;
        t.matched = true;
    }
}

// An alive track without a match this frame: one more miss; past kMaxMiss it frees its slot.
void BoxSmoother::expire()
{
    for ( auto& t : tracks_ )
    {
        if ( !t.alive || t.matched )
            continue;
        const int lim = ( t.hits >= kCoastMinHits ) ? kMaxMiss : kUnconfMaxMiss;
        if ( ++t.miss > lim )
            t.alive = false;
    }
}

// The output in input order: a matched det carries its track's smoothed box,
// an unmatched one passes through unsmoothed and spawns a track.
void BoxSmoother::emit( const DetList& in, const DetTrack& det_trk, DetList& out )
{
    for ( int i = 0; i < in.n; ++i )
    {
        const Detection& d = in.d[i];
        if ( !det_usable( d ) )
            continue;  // M-14: never smoothed, spawned or drawn
        Detection sd = d;
        if ( d.cls != 0 )
        {
            if ( i < kMaxDet && det_trk[i] >= 0 )
            {
                const BoxTrack& t = tracks_[det_trk[i]];
                if ( t.hits < kCoastMinHits && t.conf < kInstantConf )
                    continue;
                sd.x = t.x;
                sd.y = t.y;
                sd.w = t.w;
                sd.h = t.h;
            }
            else
            {
                spawn( d );
                if ( d.conf < kInstantConf )
                    continue;
            }
        }
        out.push( sd );
    }
    emit_coast( out );
}

void BoxSmoother::emit_coast( DetList& out ) const
{
    for ( const auto& t : tracks_ )
    {
        if ( t.alive && !t.matched && t.hits >= kCoastMinHits && out.n < DetList::cap )
            out.push( Detection { t.x, t.y, t.w, t.h, t.cls, t.conf } );
    }
}

// The first free slot takes the detection: the first sighting passes
// through unsmoothed (warm-up). No free slot: no track.
void BoxSmoother::spawn( const Detection& d )
{
    auto* const it = std::find_if( tracks_.begin(), tracks_.end(), []( const BoxTrack& t ) {
        return !t.alive;
    } );
    if ( it == tracks_.end() )
        return;
    BoxTrack& t = *it;
    t.filt = BoxFilter {};  // a reused slot must not inherit the previous object's state
    t.filt.reset( BoxCWH { d.x + 0.5f * d.w, d.y + 0.5f * d.h, d.w, d.h } );
    t.x = d.x;
    t.y = d.y;
    t.w = d.w;
    t.h = d.h;
    t.conf = d.conf;
    t.cls = d.cls;
    t.hits = 1;
    t.miss = 0;
    t.alive = true;
    t.matched = true;
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
