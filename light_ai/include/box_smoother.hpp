#pragma once

#include <array>
#include <fstream>
#include <string>

#include "yolo_postprocess.hpp"

// M3.10: EMA smoothing of detection boxes (owner-approved design).
// Filtering lives in Pipeline; yolo_postprocess stays a pure function.
// Greedy IoU matching (best IoU > 0.3, class must match); alpha = 0.5;
// a missed detection keeps the track (and its filtered coords) for up
// to 2 frames so a brief dropout does not reset the filter, more frees
// the slot. No heap: a fixed std::array of 32 slots and fixed-capacity
// DetList buffers (PodList — no allocations at all).
class BoxSmoother
{
public:
    void set_log( const std::string& path )
    {
        if ( !path.empty() )
            log_.open( path );  // overwrite: each A/B run starts clean
    }

    void update( const DetList& in, DetList& out )
    {
        out.clear();
        for ( auto& t : tracks_ )
            t.matched = false;
        for ( int i = 0; i < in.n; ++i )
        {
            const Detection& d = in.d[i];
            int best = -1;
            float best_iou = 0.3f;
            for ( size_t t = 0; t < tracks_.size(); ++t )
            {
                if ( !tracks_[t].alive || tracks_[t].matched || tracks_[t].cls != d.cls )
                    continue;
                const float v = iou( tracks_[t], d );
                if ( v > best_iou )
                {
                    best_iou = v;
                    best = static_cast<int>( t );
                }
            }
            Detection sd = d;
            if ( best >= 0 )
            {
                BoxTrack& t = tracks_[best];
                t.x = 0.5f * t.x + 0.5f * d.x;
                t.y = 0.5f * t.y + 0.5f * d.y;
                t.w = 0.5f * t.w + 0.5f * d.w;
                t.h = 0.5f * t.h + 0.5f * d.h;
                t.miss = 0;
                t.matched = true;
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
        for ( auto& t : tracks_ )
            if ( t.alive && !t.matched && ++t.miss > 2 )
                t.alive = false;
    }

    void log_frame( int frame, const DetList& raw, const DetList& smooth )
    {
        if ( !log_.is_open() )
            return;
        log_ << frame << ',' << raw.n << ',' << smooth.n << '\n';
        for ( int k = 0; k < raw.n; ++k )
        {
            const Detection& d = raw.d[k];
            log_ << "raw," << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y << ',' << d.w << ',' << d.h << '\n';
        }
        for ( int k = 0; k < smooth.n; ++k )
        {
            const Detection& d = smooth.d[k];
            log_ << "sm," << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y << ',' << d.w << ',' << d.h << '\n';
        }
    }

private:
    struct BoxTrack
    {
        float x = 0, y = 0, w = 0, h = 0;
        int cls = -1;
        int miss = 0;
        bool alive = false;
        bool matched = false;
    };

    float iou( const BoxTrack& t, const Detection& d ) const
    {
        const float x0 = t.x > d.x ? t.x : d.x;
        const float y0 = t.y > d.y ? t.y : d.y;
        const float x1 = ( t.x + t.w ) < ( d.x + d.w ) ? ( t.x + t.w ) : ( d.x + d.w );
        const float y1 = ( t.y + t.h ) < ( d.y + d.h ) ? ( t.y + t.h ) : ( d.y + d.h );
        const float iw = x1 - x0, ih = y1 - y0;
        if ( iw <= 0.f || ih <= 0.f )
            return 0.f;
        const float inter = iw * ih;
        const float uni = t.w * t.h + d.w * d.h - inter;
        return uni > 0.f ? inter / uni : 0.f;
    }

private:  // data (header-layout rule: functions first, then a
          // fresh visibility label for the data)
    std::array<BoxTrack, 32> tracks_ {};
    std::ofstream log_;
};
