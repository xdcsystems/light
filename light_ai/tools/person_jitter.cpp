// tools/person_jitter.cpp -- the jitter of the displayed person box on the Layer A
// replay (#97 item 4). Layer A (reference/ref_evening.csv, the --box-log of the
// replay) has no person display boxes: BoxSmoother skips cls 0, and Layer B
// (tests/test_reference_chain.cpp) stops at the tracker output (post == trajs).
// This probe feeds every `raw` line of the CSV through the real chain of the
// person boxes -- Tracker (raw detections, as Pipeline::publish_oldest() does) and the
// OverlaySink display filter, TrackBoxFilters with box_filter::kTrackBoxParams --
// and prints the frame-to-frame jitter of a CONF track's box before the filter
// (the Kalman posterior) and after it (the drawn box).
//
// The metric is the one of the static-chair figure of 6dc6a14 (docs/state.md):
// for two consecutive frames, max over x, y, w, h (top-left box, scene px) of
// |a - b|; mean and nearest-rank p90. The probe recomputes the chair figure from
// the `sm` lines of the same CSV with the same code (one box of the class in both
// frames): it must give chair (56) 0.95 / 1.87 px, which proves the metric.
// A person pair counts as still when the posterior speed of the later frame is
// below kStillPxS.
//
// Timestamps: frame * kStepUs, the clip's frame period (as Layer B). Host only,
// no SDK, not in the gate. Disposable probe: tools/ is exempt from the statics
// rule and owns its std::cout.
//
// Build and run (from light_ai/):
//   g++ -std=gnu++17 -O2 -Wall -Wextra -Iinclude -o build/person_jitter tools/person_jitter.cpp
//       src/core/tracker.cpp src/core/associator.cpp src/core/trail_window.cpp   (one command line)
//   ./build/person_jitter reference/ref_evening.csv

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/box_filter.hpp"
#include "core/detection.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"

namespace
{
    constexpr int64_t kStepUs = 47037;  // the clip's frame period: 1063 frames in 50 s (reference/README.md)
    constexpr float kStillPxS = 20.0f;  // a still person: posterior speed below 20 px/s (~0.9 px per frame)

    struct Frame
    {
        int frame = -1;
        std::vector<Detection> raw;
        std::vector<Detection> sm;
    };

    struct Stats
    {
        std::vector<float> d;
        void add( float v ) { d.push_back( v ); }
        float mean() const
        {
            double s = 0.0;
            for ( const float v : d )
                s += v;
            return d.empty() ? 0.0f : static_cast<float>( s / static_cast<double>( d.size() ) );
        }
        float p90() const  // nearest rank
        {
            if ( d.empty() )
                return 0.0f;
            std::vector<float> v = d;
            std::sort( v.begin(), v.end() );
            const auto rank = static_cast<std::size_t>( std::ceil( 0.9 * static_cast<double>( v.size() ) ) );
            return v[std::max<std::size_t>( rank, 1 ) - 1];
        }
        int frozen() const  // pairs with no visible change at all
        {
            return static_cast<int>( std::count_if( d.begin(), d.end(), []( float v ) { return v < 1e-3f; } ) );
        }
    };

    // One field of p up to sep (',' or the end): a strict strtol / strtof.
    bool field( const char*& p, long& v, char sep )
    {
        char* end = nullptr;
        errno = 0;
        v = std::strtol( p, &end, 10 );
        const bool ok = end != p && errno == 0 && *end == sep;
        p = sep != '\0' && ok ? end + 1 : end;
        return ok;
    }
    bool field( const char*& p, float& v, char sep )
    {
        char* end = nullptr;
        errno = 0;
        v = std::strtof( p, &end );
        const bool ok = end != p && errno == 0 && *end == sep;
        p = sep != '\0' && ok ? end + 1 : end;
        return ok;
    }

    // "kind,cls,conf,x,y,w,h" -> d; false on another line.
    bool box_line( const std::string& line, const char* kind, Detection& d )
    {
        const std::string pre = std::string( kind ) + ",";
        if ( line.compare( 0, pre.size(), pre ) != 0 )
            return false;
        const char* p = line.c_str() + pre.size();
        long cls = 0;
        const bool ok = field( p, cls, ',' ) && field( p, d.conf, ',' ) && field( p, d.x, ',' ) && field( p, d.y, ',' ) &&
                        field( p, d.w, ',' ) && field( p, d.h, '\0' );
        d.cls = static_cast<int>( cls );
        return ok;
    }

    // A frame header "frame,n_raw,n_sm" opens a frame; its raw / sm lines follow.
    bool read_csv( const char* path, std::vector<Frame>& out )
    {
        std::ifstream in( path );
        if ( !in )
            return false;
        std::string line;
        while ( std::getline( in, line ) )
        {
            Detection d {};
            if ( box_line( line, "raw", d ) )
            {
                if ( out.empty() )
                    return false;
                out.back().raw.push_back( d );
                continue;
            }
            if ( box_line( line, "sm", d ) )
            {
                if ( out.empty() )
                    return false;
                out.back().sm.push_back( d );
                continue;
            }
            const char* p = line.c_str();
            long frame = 0;
            if ( !field( p, frame, ',' ) )
                return false;
            out.push_back( Frame { static_cast<int>( frame ), {}, {} } );
        }
        return !out.empty();
    }

    float box_delta( float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh )
    {
        return std::max( { std::fabs( ax - bx ), std::fabs( ay - by ), std::fabs( aw - bw ), std::fabs( ah - bh ) } );
    }

    float track_delta( const TrackState& a, const TrackState& b )
    {
        return box_delta( a.cx - 0.5f * a.bw, a.cy - 0.5f * a.bh, a.bw, a.bh, b.cx - 0.5f * b.bw, b.cy - 0.5f * b.bh, b.bw,
                          b.bh );
    }

    const Detection* only_of( const std::vector<Detection>& v, int cls )
    {
        const Detection* hit = nullptr;
        for ( const Detection& d : v )
        {
            if ( d.cls != cls )
                continue;
            if ( hit != nullptr )
                return nullptr;  // two boxes of the class: no pair
            hit = &d;
        }
        return hit;
    }

    void print( const char* what, const Stats& s )
    {
        std::printf( "  %-34s pairs %4zu  mean %5.2f px  p90 %5.2f px  frozen %4d\n", what, s.d.size(), s.mean(), s.p90(),
                     s.frozen() );
    }

    // The static-object figure of 6dc6a14 from the sm lines.
    void static_figure( const std::vector<Frame>& fr, int cls )
    {
        Stats s;
        for ( std::size_t k = 1; k < fr.size(); ++k )
        {
            if ( fr[k].frame != fr[k - 1].frame + 1 )
                continue;
            const Detection* a = only_of( fr[k - 1].sm, cls );
            const Detection* b = only_of( fr[k].sm, cls );
            if ( a != nullptr && b != nullptr )
                s.add( box_delta( a->x, a->y, a->w, a->h, b->x, b->y, b->w, b->h ) );
        }
        const std::string what = "cls " + std::to_string( cls ) + ", sm lines";
        print( what.c_str(), s );
    }

    const TrackState* find_conf( const TrajList& l, int id, uint32_t gen )
    {
        for ( int k = 0; k < l.n; ++k )
            if ( l.d[k].id == id && l.d[k].gen == gen && l.d[k].state == TRACK_CONF )
                return &l.d[k];
        return nullptr;
    }

    struct PersonStats
    {
        Stats pre_all;
        Stats post_all;
        Stats pre_still;
        Stats post_still;
    };

    // The CONF tracks of this frame that were CONF in the previous one: one pair each.
    void add_pairs( const TrajList& trajs, const TrajList& disp, const TrajList& prev_trajs, const TrajList& prev_disp,
                    PersonStats& st )
    {
        for ( int k = 0; k < disp.n; ++k )
        {
            const TrackState& b_pre = trajs.d[k];
            const TrackState& b_post = disp.d[k];
            if ( b_pre.state != TRACK_CONF )
                continue;
            const TrackState* a_pre = find_conf( prev_trajs, b_pre.id, b_pre.gen );
            const TrackState* a_post = find_conf( prev_disp, b_pre.id, b_pre.gen );
            if ( a_pre == nullptr || a_post == nullptr )
                continue;
            const float d_pre = track_delta( *a_pre, b_pre );
            const float d_post = track_delta( *a_post, b_post );
            st.pre_all.add( d_pre );
            st.post_all.add( d_post );
            if ( std::hypot( b_pre.vx, b_pre.vy ) >= kStillPxS )
                continue;
            st.pre_still.add( d_pre );
            st.post_still.add( d_post );
        }
    }

    // Tracker on the raw lines (as Pipeline::publish_oldest()), then the OverlaySink display filter.
    void person_figure( const std::vector<Frame>& fr )
    {
        Tracker tracker;
        TrackBoxFilters filt;
        TrajList trajs {};
        TrajList disp {};
        TrajList prev_trajs {};
        TrajList prev_disp {};
        PersonStats st;
        int prev_frame = -2;
        for ( const Frame& f : fr )
        {
            DetList raw {};
            for ( const Detection& d : f.raw )
                raw.push( d );
            const int64_t ts = static_cast<int64_t>( f.frame ) * kStepUs;
            tracker.update( raw, ts, trajs );
            disp = trajs;
            filt.apply( disp, ts, box_filter::kTrackBoxParams );
            if ( f.frame == prev_frame + 1 )
                add_pairs( trajs, disp, prev_trajs, prev_disp, st );
            prev_trajs = trajs;
            prev_disp = disp;
            prev_frame = f.frame;
        }
        print( "person CONF, posterior (pre)", st.pre_all );
        print( "person CONF, displayed (post)", st.post_all );
        std::printf( "  still: posterior speed < %.0f px/s\n", static_cast<double>( kStillPxS ) );
        print( "person CONF still, posterior (pre)", st.pre_still );
        print( "person CONF still, displayed (post)", st.post_still );
    }
}  // namespace

int main( int argc, char** argv )
{
    if ( argc != 2 )
    {
        std::cerr << "usage: person_jitter <box-log.csv>\n";
        return 2;
    }
    std::vector<Frame> fr;
    if ( !read_csv( argv[1], fr ) )
    {
        std::cerr << "person_jitter: cannot read " << argv[1] << '\n';
        return 1;
    }
    std::printf( "%s: %zu frames, frame-to-frame jitter, max over x, y, w, h (scene px)\n", argv[1], fr.size() );
    static_figure( fr, 56 );
    static_figure( fr, 58 );
    person_figure( fr );
    return 0;
}
