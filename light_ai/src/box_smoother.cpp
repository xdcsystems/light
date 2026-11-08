#include <algorithm>  // the deterministic pair order
#include <iostream>   // set_log failure note

#include "box_smoother.h"

void BoxSmoother::set_log( const std::string& path )
{
    if ( path.empty() )
        return;
    log_.open( path );  // overwrite: each A/B run starts clean
    if ( !log_.is_open() )
        std::cerr << "smoother: box-log open failed for '"
                  << path << "' - A/B logging disabled\n";
}

void BoxSmoother::update( const DetList& in, DetList& out )
{
    out.clear();
    for ( auto& t : tracks_ )
        t.matched = false;

    // The in-order scan let the first detection steal a track just over
    // the threshold while a much better match arriving later got
    // the leftovers. All pairs are collected, sorted by IoU (descending)
    // and assigned best-first - the standard greedy bipartite matching.
    // Semantics otherwise unchanged: warm-up pass-through, miss expiry, out order.
    
    int np = 0;  // pairs_ is the preallocated member (data section)
    std::array<int, kMaxDet> det_trk {};
    det_trk.fill( -1 );

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
                pairs_[np++] = { v, i, t };
        }
    }
    // Deterministic order: IoU desc, then det index, then track index.
    std::sort( pairs_.begin(), pairs_.begin() + np, []( const Pair& a, const Pair& b ) {
        return a.iou != b.iou ? a.iou > b.iou : ( a.det != b.det ? a.det < b.det : a.trk < b.trk );
    } );
    for ( int p = 0; p < np; ++p )
    {
        const Pair& pr = pairs_[p];
        if ( det_trk[pr.det] >= 0 || tracks_[pr.trk].matched )
            continue;
        det_trk[pr.det] = pr.trk;
        BoxTrack& t = tracks_[pr.trk];
        const Detection& d = in.d[pr.det];
        t.x = kAlpha * t.x + ( 1.0f - kAlpha ) * d.x;
        t.y = kAlpha * t.y + ( 1.0f - kAlpha ) * d.y;
        t.w = kAlpha * t.w + ( 1.0f - kAlpha ) * d.w;
        t.h = kAlpha * t.h + ( 1.0f - kAlpha ) * d.h;
        t.miss = 0;
        t.matched = true;
    }

    for ( auto& t : tracks_ )
        if ( t.alive && !t.matched && ++t.miss > kMaxMiss )
            t.alive = false;

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
                break;  // first sighting passes through unsmoothed (warm-up)
            }
        }
        out.push( sd );
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

float BoxSmoother::iou( const BoxTrack& t, const Detection& d ) const
{
    const float x0 = t.x > d.x ? t.x : d.x;
    const float y0 = t.y > d.y ? t.y : d.y;
    const float x1 = ( t.x + t.w ) < ( d.x + d.w ) ? ( t.x + t.w ) : ( d.x + d.w );
    const float y1 = ( t.y + t.h ) < ( d.y + d.h ) ? ( t.y + t.h ) : ( d.y + d.h );

    const float iw = x1 - x0;
    const float ih = y1 - y0;

    if ( iw <= 0.f || ih <= 0.f )
        return 0.f;

    const float inter = iw * ih;
    const float uni = t.w * t.h + d.w * d.h - inter;
    return uni > 0.f ? inter / uni : 0.f;
}
