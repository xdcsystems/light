// BoxFilter: the one display filter of every drawn box (#91 item 4; replaces
// BoxEnvelope in OverlaySink and BoxEnvelope + EMA in BoxSmoother).
//
// Two stages per box axis (cx, cy, w, h):
//   1. a One-Euro low-pass (Casiez, Roussel, Vogel, CHI 2012): the cutoff rises
//      with the axis speed, so a still box is smoothed hard and a moving one
//      follows with little lag;
//   2. a hysteresis dead zone (backlash): the output stays where it is while the
//      filtered value is within +-dz of it, and is dragged along at dz distance
//      once the filtered value pushes past. A standing box is rock-still (the
//      filtered jitter never reaches the edge), a moving box moves at the filtered
//      speed -- no lock / unlock state, so no freeze-then-jump on a slow walk and
//      no catch-up burst when motion starts (the BoxEnvelope faults of audit M-3).
//      The price: a moving box trails its filtered value by dz, and a box that
//      came to rest may sit up to dz off it.
// dz of an axis = max(dz_min_px, dz_frac * box size): cx and w scale with the
// width, cy and h with the height.
// Time-based (dt in seconds): variable frame rates are fine. A non-finite or
// empty input box, or dt <= 0, leaves the output as it was; a gap above max_dt_s
// restarts the filter at the input.
//
// TrackBoxFilters keeps one BoxFilter per live track of one source, keyed by
// (TrackState::id, gen): a track it did not see in the previous call starts
// fresh. Pure logic, no heap; the caller's thread only.

#pragma once

#include <algorithm>  // clamp, max, find_if
#include <cmath>      // fabs, isfinite
#include <cstdint>

#include "core/track.hpp"  // TrajList, TrackState
#include "pod_list.hpp"

struct BoxFilterParams
{
    float min_cutoff_hz = 0.5f;  // cutoff of a still axis
    float beta = 0.01f;          // cutoff gain, Hz per px/s of the filtered axis speed
    float d_cutoff_hz = 1.0f;    // cutoff of the speed estimate
    float dz_frac = 0.01f;       // dead zone: fraction of the box size on the axis...
    float dz_min_px = 2.0f;      // ...but at least this many px
    float max_dt_s = 0.5f;       // a longer gap restarts the filter at the input
};

// Center + size box (the TrackState convention).
struct BoxCWH
{
    float cx = 0.0f;
    float cy = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

class BoxFilter
{
public:
    enum
    {
        kAxes = 4
    };

    static bool usable( const BoxCWH& b ) noexcept
    {
        return std::isfinite( b.cx ) && std::isfinite( b.cy ) && std::isfinite( b.w ) && std::isfinite( b.h ) && b.w > 0.0f &&
               b.h > 0.0f;
    }

    // Starts at b (output = b). An unusable b is ignored.
    void reset( const BoxCWH& b ) noexcept
    {
        if ( !usable( b ) )
            return;
        const float v[kAxes] = { b.cx, b.cy, b.w, b.h };
        for ( int i = 0; i < kAxes; ++i )
        {
            ax_[i] = Axis { v[i], 0.0f };
            out_[i] = v[i];
        }
        primed_ = true;
    }

    // One input box, dt_s after the previous one. The first usable box primes the filter.
    void step( const BoxCWH& b, float dt_s, const BoxFilterParams& p ) noexcept
    {
        if ( !usable( b ) )
            return;
        if ( !primed_ || dt_s > p.max_dt_s )
        {
            reset( b );
            return;
        }
        if ( !std::isfinite( dt_s ) || !( dt_s > 0.0f ) )
            return;
        const float v[kAxes] = { b.cx, b.cy, b.w, b.h };
        float f[kAxes] {};
        for ( int i = 0; i < kAxes; ++i )
            f[i] = one_euro( ax_[i], v[i], dt_s, p );
        for ( int i = 0; i < kAxes; ++i )
        {
            const float dz = dead_zone( i, f, p );
            out_[i] = std::clamp( out_[i], f[i] - dz, f[i] + dz );
        }
    }

    BoxCWH out() const noexcept { return BoxCWH { out_[0], out_[1], out_[2], out_[3] }; }
    bool primed() const noexcept { return primed_; }

    // Dead zone of axis i around box f (cx, cy, w, h): cx and w scale with the width, cy and h with the height.
    static float dead_zone( int i, const float f[kAxes], const BoxFilterParams& p ) noexcept
    {
        const float size = ( i == 0 || i == 2 ) ? f[2] : f[3];
        return std::max( p.dz_min_px, p.dz_frac * size );
    }

private:
    struct Axis
    {
        float x = 0.0f;   // filtered value
        float dx = 0.0f;  // filtered speed, units/s
    };

    // The One-Euro smoothing factor of a first-order low-pass at cutoff_hz.
    static float alpha( float cutoff_hz, float dt_s ) noexcept
    {
        constexpr float two_pi = 6.2831853f;
        const float r = two_pi * cutoff_hz * dt_s;
        return r / ( r + 1.0f );
    }

    static float one_euro( Axis& a, float x, float dt_s, const BoxFilterParams& p ) noexcept
    {
        const float dx_raw = ( x - a.x ) / dt_s;
        a.dx += alpha( p.d_cutoff_hz, dt_s ) * ( dx_raw - a.dx );
        const float fc = p.min_cutoff_hz + p.beta * std::fabs( a.dx );
        a.x += alpha( fc, dt_s ) * ( x - a.x );
        return a.x;
    }

private:
    Axis ax_[kAxes] {};
    float out_[kAxes] {};
    bool primed_ = false;
};

class TrackBoxFilters
{
public:
    void clear() noexcept { list_.clear(); }

    // Filters the box (cx, cy, bw, bh) of every TENT / CONF track of tracks in
    // place; other states pass through and drop their filter. ts_us: the frame time.
    void apply( TrajList& tracks, int64_t ts_us, const BoxFilterParams& p ) noexcept
    {
        next_.clear();
        for ( int k = 0; k < tracks.n; ++k )
        {
            TrackState& tr = tracks.d[k];
            if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
                continue;
            const BoxCWH raw { tr.cx, tr.cy, tr.bw, tr.bh };
            Entry e { tr.id, tr.gen, ts_us, BoxFilter {} };
            const Entry* prev = find( tr.id, tr.gen );
            if ( prev != nullptr )
            {
                e.f = prev->f;
                e.f.step( raw, static_cast<float>( ts_us - prev->ts_us ) * 1e-6f, p );
            }
            else
            {
                e.f.reset( raw );
            }
            if ( e.f.primed() )
            {
                const BoxCWH o = e.f.out();
                tr.cx = o.cx;
                tr.cy = o.cy;
                tr.bw = o.w;
                tr.bh = o.h;
            }
            next_.push( e );
        }
        list_ = next_;
    }

private:
    struct Entry
    {
        int id = 0;
        uint32_t gen = 0;
        int64_t ts_us = 0;
        BoxFilter f {};
    };

    const Entry* find( int id, uint32_t gen ) const noexcept
    {
        const auto* const end = list_.d.begin() + list_.n;
        const auto* const it = std::find_if( list_.d.begin(), end, [id, gen]( const Entry& e ) {
            return e.id == id && e.gen == gen;
        } );
        return it != end ? it : nullptr;
    }

private:
    PodList<Entry, TrajList::cap> list_ {};
    PodList<Entry, TrajList::cap> next_ {};
};
