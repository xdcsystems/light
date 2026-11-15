// Golden test of the time-dependent half of the reference detections (issue #32,
// layer B; reference/README.md). Recorded raw detections of the reference clip
// (tests/data/ref_evening_raw.csv, a slice of reference/ref_evening.csv) go through
// the chain of Pipeline::publish_oldest() + OverlaySink with FIXED timestamps:
//   BoxSmoother::update( raw -> sm ), Tracker::update( raw, ts -> tracks ),
//   BoxMapper::fill( sm, tracks, tracker, boxes, lb, sx, sy, ts ) at the production
//   geometry (1280x960 into the 640x640 model: band_geom, sx = lb.w / 1280).
// Two synthetic segments follow, detections generated in code, each through a
// fresh chain: "crossing" (two persons walk through each other, one occluded for
// 3 frames: the association arbitrates between two tracks) and "weak" (a merged
// blob only the round-2 gates accept). The clip has one person at a time, so
// without them greedy_iou_match never arbitrates and round 2 never matches.
// Their scenario CHECKs (ids kept, the track survives alone) state the intent;
// the golden pins the numbers.
// The output (smoothed boxes, every TrackState, every GpuBox; an "h" line opens
// each segment) is compared with tests/data/ref_evening_golden.txt, written by
// the tracker code of fix/audit-p0 @ f3c5a7f (tests/data/README).
//
// Compare: line structure, counts, ids, states, classes, colors and alpha exact;
// floats within a tolerance -- positions / sizes in px (kTolPx), velocities in
// px/s (kTolPxs), confidences (kTolConf). Why a tolerance: the build is gnu++17, where GCC contracts
// a*b+c into a fused multiply-add (-ffp-contract=fast); aarch64 always has FMA,
// the x86 baseline does not, so the same code differs in the last bits between
// the board and an x86 host. The tolerances sit well above the measured
// spread (tests/data/README) and well below a real change: the kQAccel
// 2000 -> 2100 nudge, round 2 switched off and a worst-first greedy order each
// fail this test.
//
// Timestamps: fixed, kStepUs per frame block (the synthetic segments: frame x kStepUs); where the fixture's frame numbers
// jump (the fixture is cut from separate stretches of the clip), the step grows by
// kJumpUs -- a source stall that lost frames, longer than the tracker's dt clamp
// (kDtMaxMs 500) and its tentative patience (kTentMaxAgeMs 1000). The clip itself
// never kills a tentative track (every spawn is confirmed by the next frames), so
// the cut sits right after a spawn. The test asserts its own coverage (a dt
// clamp, a tentative death, a DEAD ghost, a trail-point eviction): a regenerated
// fixture cannot silently lose what the test guards.
//
// LIGHT_AI_WRITE_GOLDEN=1 rewrites the golden instead of comparing -- only for a
// deliberate behavior change (never in a refactoring), see tests/data/README.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "box_mapper.h"
#include "core/box_math.hpp"
#include "box_smoother.h"
#include "check.hpp"
#include "core/geometry.hpp"
#include "suites.hpp"
#include "tracker.hpp"

#ifndef LIGHT_AI_TEST_DATA_DIR
#error "LIGHT_AI_TEST_DATA_DIR is set by tests/CMakeLists.txt"
#endif

namespace
{
    constexpr int64_t kStepUs = 47037;      // the clip's frame period: 1063 frames in 50 s (reference/README.md)
    constexpr int64_t kJumpUs = 1200000;    // the stall at a cut of the fixture
    constexpr int64_t kDtClampUs = 500000;  // Tracker kDtMaxMs, for the coverage check only
    enum : int
    {
        kSrcW = 1280,
        kSrcH = 960,
        kModel = 640,
        kMaxReport = 5,  // mismatching lines printed at most
    };
    constexpr float kTolPx = 1e-2f;
    constexpr float kTolPxs = 1e-1f;
    constexpr float kTolConf = 1e-4f;

    // The fixture's next frame: "frame,n_raw,n_sm" then n_raw "raw,cls,conf,x,y,w,h" lines.
    bool next_frame( std::istream& in, int& frame, DetList& raw )
    {
        raw.clear();
        std::string line;
        if ( !std::getline( in, line ) || line.empty() )
            return false;
        int n = 0;
        if ( std::sscanf( line.c_str(), "%d,%d", &frame, &n ) != 2 )
            return false;
        for ( int k = 0; k < n; ++k )
        {
            if ( !std::getline( in, line ) )
                return false;
            Detection d {};
            if ( std::sscanf( line.c_str(), "raw,%d,%f,%f,%f,%f,%f", &d.cls, &d.conf, &d.x, &d.y, &d.w, &d.h ) != 6 )
                return false;
            raw.push( d );
        }
        return true;
    }

    void put( std::ostream& os, const char* fmt, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
    void put( std::ostream& os, const char* fmt, ... )
    {
        char buf[256];
        va_list ap;
        va_start( ap, fmt );
        std::vsnprintf( buf, sizeof( buf ), fmt, ap );
        va_end( ap );
        os << buf << '\n';
    }

    // Coverage of the tracker paths the golden is meant to guard.
    struct Coverage
    {
        int dt_clamps = 0;     // a predict across a gap above the clamp
        int tent_deaths = 0;   // TENT in one frame, DEAD in the next
        int ghosts = 0;        // DEAD track states served
        int evictions = 0;     // a track's n_pts went down: trail points aged out
    };

    const TrackState* find_track( const TrajList& l, int id )
    {
        for ( int k = 0; k < l.n; ++k )
            if ( l.d[k].id == id )
                return &l.d[k];
        return nullptr;
    }

    void cover( const TrajList& prev, const TrajList& cur, int64_t dt_us, Coverage& c )
    {
        bool any_alive = false;
        for ( int k = 0; k < prev.n; ++k )
            any_alive = any_alive || prev.d[k].state == TRACK_TENT || prev.d[k].state == TRACK_CONF;
        if ( any_alive && dt_us > kDtClampUs )
            ++c.dt_clamps;
        for ( int k = 0; k < cur.n; ++k )
        {
            const TrackState& s = cur.d[k];
            const TrackState* p = find_track( prev, s.id );
            c.ghosts += s.state == TRACK_DEAD ? 1 : 0;
            if ( p == nullptr )
                continue;
            c.tent_deaths += ( p->state == TRACK_TENT && s.state == TRACK_DEAD ) ? 1 : 0;
            c.evictions += s.n_pts < p->n_pts ? 1 : 0;
        }
    }

    // One output block per frame:
    //   f <frame> <ts_us> <n_sm> <n_tracks> <n_boxes>
    //   s <cls> <conf> <x> <y> <w> <h>                 smoothed detections
    //   t <id> <state> <cx> <cy> <vx> <vy> <bw> <bh> <n_pts>
    //   b <x0> <y0> <x1> <y1> <r> <g> <b> <a>
    void write_frame( std::ostream& out, int frame, int64_t ts, const DetList& sm, const TrajList& tracks,
                      const GpuBoxList& boxes )
    {
        put( out, "f %d %lld %d %d %d", frame, static_cast<long long>( ts ), sm.n, tracks.n, boxes.n );
        for ( int k = 0; k < sm.n; ++k )
        {
            const Detection& d = sm.d[k];
            put( out, "s %d %.6f %.4f %.4f %.4f %.4f", d.cls, d.conf, d.x, d.y, d.w, d.h );
        }
        for ( int k = 0; k < tracks.n; ++k )
        {
            const TrackState& s = tracks.d[k];
            put( out, "t %d %d %.4f %.4f %.4f %.4f %.4f %.4f %d", s.id, static_cast<int>( s.state ), s.cx, s.cy, s.vx,
                 s.vy, s.bw, s.bh, s.n_pts );
        }
        for ( int k = 0; k < boxes.n; ++k )
        {
            const GpuBox& b = boxes.d[k];
            put( out, "b %.4f %.4f %.4f %.4f %d %d %d %d", b.x0, b.y0, b.x1, b.y1, b.r, b.g, b.b, b.a );
        }
    }

    // The chain of Pipeline::publish_oldest() + OverlaySink for one source, fresh
    // state per segment; every step writes its frame block.
    class Chain
    {
    public:
        // One frame: smoother, tracker, box mapper -- the production order.
        void step( int frame, int64_t ts, const DetList& raw, std::ostream& out )
        {
            smoother_.update( raw, sm_ );
            tracker_.update( raw, ts, tracks_ );
            mapper_.fill( sm_, tracks_, tracker_, boxes_, lb_, sx_, sy_, ts );
            if ( frames_ > 0 )
                cover( prev_, tracks_, ts - prev_ts_, cov_ );
            prev_ = tracks_;
            prev_ts_ = ts;
            ++frames_;
            write_frame( out, frame, ts, sm_, tracks_, boxes_ );
        }
        // The tracker output of the last step (before a step: the previous frame's).
        const TrajList& tracks() const { return tracks_; }
        const Coverage& coverage() const { return cov_; }
        int frames() const { return frames_; }

    private:
        BoxSmoother smoother_;
        BoxMapper mapper_;
        Tracker tracker_;
        DetList sm_;
        TrajList tracks_;
        TrajList prev_;
        GpuBoxList boxes_;
        LbGeom lb_ = band_geom( kSrcW, kSrcH, kModel, kModel );
        float sx_ = static_cast<float>( lb_.w ) / static_cast<float>( kSrcW );
        float sy_ = static_cast<float>( lb_.h ) / static_cast<float>( kSrcH );
        Coverage cov_;
        int frames_ = 0;
        int64_t prev_ts_ = 0;
    };

    // The recorded segment: the fixture through the chain; returns its coverage.
    Coverage run_clip( std::istream& in, std::ostream& out, int& frames )
    {
        Chain ch;
        DetList raw;
        int frame = 0;
        int prev_frame = 0;
        int64_t ts = 0;
        put( out, "h clip" );
        while ( next_frame( in, frame, raw ) )
        {
            if ( ch.frames() > 0 )
                ts += kStepUs + ( frame != prev_frame + 1 ? kJumpUs : 0 );
            prev_frame = frame;
            ch.step( frame, ts, raw, out );
        }
        frames = ch.frames();
        return ch.coverage();
    }

    Detection person( float cx, float cy, float w, float h, float conf )
    {
        return Detection { cx - w * 0.5f, cy - h * 0.5f, w, h, 0, conf };
    }

    bool alive( const TrackState& s )
    {
        return s.state == TRACK_TENT || s.state == TRACK_CONF;
    }

    // The alive track whose center is nearest (cx, cy); nullptr when none.
    const TrackState* nearest( const TrajList& l, float cx, float cy )
    {
        const TrackState* best = nullptr;
        float best_d = 0.0f;
        for ( int k = 0; k < l.n; ++k )
        {
            const TrackState& s = l.d[k];
            const float d = ( s.cx - cx ) * ( s.cx - cx ) + ( s.cy - cy ) * ( s.cy - cy );
            if ( alive( s ) && ( best == nullptr || d < best_d ) )
            {
                best = &s;
                best_d = d;
            }
        }
        return best;
    }

    float iou_with( const Detection& d, const TrackState& s )
    {
        return box_iou( d.x, d.y, d.w, d.h, s.cx - s.bw * 0.5f, s.cy - s.bh * 0.5f, s.bw, s.bh );
    }

    // Detections that overlap two or more alive tracks of the previous frame at
    // IoU >= kContestIoU: the association has to arbitrate (greedy_iou_match over
    // more than one candidate pair). Measured on the previous frame's track boxes;
    // the margin over the tracker's gate (kIoUGate 0.30) covers the predict step.
    bool contested( const DetList& raw, const TrajList& prev )
    {
        constexpr float kContestIoU = 0.4f;
        for ( int i = 0; i < raw.n; ++i )
        {
            int n = 0;
            for ( int k = 0; k < prev.n; ++k )
                n += alive( prev.d[k] ) && iou_with( raw.d[i], prev.d[k] ) >= kContestIoU ? 1 : 0;
            if ( n >= 2 )
                return true;
        }
        return false;
    }

    // Synthetic segment 1, crossing: A walks right, B walks left, 8 px per frame each,
    // slightly different sizes and heights; they overlap around frame 27, where B's
    // detection is missing for 3 frames (occluded), then they separate. Asserts:
    // the association arbitrated (contested frames) and the ids did not swap.
    void run_crossing( std::ostream& out, TestRun& t, int& contested_frames )
    {
        enum : int
        {
            kFrames = 56,
            kHideFrom = 26,  // B occluded: frames 26..28
            kHideTo = 28,
        };
        constexpr float kStep = 8.0f;
        Chain ch;
        DetList raw;
        int id_a = -1;
        int id_b = -1;
        put( out, "h crossing" );
        for ( int k = 0; k < kFrames; ++k )
        {
            const float ax = 420.0f + kStep * static_cast<float>( k );
            const float bx = 860.0f - kStep * static_cast<float>( k );
            raw.clear();
            raw.push( person( ax, 520.0f, 110.0f, 260.0f, 0.86f ) );
            if ( k < kHideFrom || k > kHideTo )
                raw.push( person( bx, 500.0f, 100.0f, 240.0f, 0.74f ) );
            contested_frames += contested( raw, ch.tracks() ) ? 1 : 0;
            ch.step( k, k * kStepUs, raw, out );
            const TrackState* a = nearest( ch.tracks(), ax, 520.0f );
            const TrackState* b = nearest( ch.tracks(), bx, 500.0f );
            if ( k == 0 && a != nullptr && b != nullptr )
            {
                id_a = a->id;
                id_b = b->id;
            }
        }
        const float a_end = 420.0f + kStep * ( kFrames - 1 );
        const float b_end = 860.0f - kStep * ( kFrames - 1 );
        const TrackState* a = nearest( ch.tracks(), a_end, 520.0f );
        const TrackState* b = nearest( ch.tracks(), b_end, 500.0f );
        CHECK( t, id_a > 0 && id_b > 0 && id_a != id_b );
        CHECK( t, a != nullptr && a->id == id_a && a->state == TRACK_CONF );  // no swap
        CHECK( t, b != nullptr && b->id == id_b && b->state == TRACK_CONF );
        CHECK( t, contested_frames >= 1 );
    }

    // Synthetic segment 2, round 2 (ByteTrack-style second pass): a standing person
    // confirmed for 10 frames, then 12 frames of a merged-blob detection 2.2x its
    // size on the same center -- size ratio 220 % (above kSizeMax 200, within
    // kSizeMaxWeak 300), IoU 0.21 (below kIoUGate 0.30, above kIoUGateWeak 0.15):
    // only round 2 can match it. Without round 2 the track misses, dies after
    // kMaxMiss (4) and the blob spawns a new id. Asserts: every blob frame meets the
    // weak-only conditions, and the track survives under the same id, alone.
    void run_weak( std::ostream& out, TestRun& t, int& weak_frames )
    {
        enum : int
        {
            kFrames = 28,
            kBlobFrom = 10,
            kBlobTo = 21,
        };
        constexpr float kCx = 640.0f;
        constexpr float kCy = 500.0f;
        constexpr float kW = 120.0f;
        constexpr float kH = 300.0f;
        constexpr float kBlob = 2.2f;
        Chain ch;
        DetList raw;
        int id = -1;
        put( out, "h weak" );
        for ( int k = 0; k < kFrames; ++k )
        {
            const bool blob = k >= kBlobFrom && k <= kBlobTo;
            const float g = blob ? kBlob : 1.0f;
            raw.clear();
            raw.push( person( kCx, kCy, kW * g, kH * g, blob ? 0.41f : 0.88f ) );
            const TrackState* p = nearest( ch.tracks(), kCx, kCy );
            if ( blob && p != nullptr && p->state == TRACK_CONF )
            {
                const float rw = raw.d[0].w / p->bw;
                const float rh = raw.d[0].h / p->bh;
                const float v = iou_with( raw.d[0], *p );
                const bool weak_only = rw > 2.0f && rw <= 3.0f && rh > 2.0f && rh <= 3.0f && v >= 0.15f && v < 0.30f;
                CHECK( t, weak_only );
                weak_frames += weak_only ? 1 : 0;
            }
            ch.step( k, k * kStepUs, raw, out );
            int n_alive = 0;
            for ( int i = 0; i < ch.tracks().n; ++i )
                n_alive += alive( ch.tracks().d[i] ) ? 1 : 0;
            const TrackState* s = nearest( ch.tracks(), kCx, kCy );
            if ( k == 0 && s != nullptr )
                id = s->id;
            if ( k >= kBlobFrom )
                CHECK( t, s != nullptr && s->id == id && s->state == TRACK_CONF && n_alive == 1 );
        }
        CHECK( t, weak_frames == kBlobTo - kBlobFrom + 1 );
    }

    // Token classes per line kind: i = exact, p = px (kTolPx), v = px/s (kTolPxs), c = conf (kTolConf).
    const char* token_classes( char kind )
    {
        switch ( kind )
        {
            case 'h':
                return "i";
            case 'f':
                return "iiiii";
            case 's':
                return "icpppp";
            case 't':
                return "iippvvppi";
            case 'b':
                return "ppppiiii";
            default:
                return nullptr;
        }
    }

    struct Deviation
    {
        double px = 0.0;
        double pxs = 0.0;
        double conf = 0.0;
    };

    // One line against its golden twin; updates the max deviations.
    bool same_line( const std::string& got, const std::string& want, Deviation& dev )
    {
        if ( got.empty() || want.empty() || got[0] != want[0] )
            return false;
        const char* cls = token_classes( got[0] );
        if ( cls == nullptr )
            return false;
        std::istringstream g( got.substr( 1 ) );
        std::istringstream w( want.substr( 1 ) );
        bool ok = true;
        for ( const char* c = cls; *c != '\0'; ++c )
        {
            std::string a;
            std::string b;
            if ( !( g >> a ) || !( w >> b ) )
                return false;
            if ( *c == 'i' )
            {
                ok = ok && a == b;
                continue;
            }
            const double d = std::fabs( std::strtod( a.c_str(), nullptr ) - std::strtod( b.c_str(), nullptr ) );
            double& mx = *c == 'p' ? dev.px : *c == 'v' ? dev.pxs : dev.conf;
            mx = std::max( mx, d );
            ok = ok && d <= static_cast<double>( *c == 'p' ? kTolPx : *c == 'v' ? kTolPxs : kTolConf );
        }
        std::string extra;
        return ok && !( g >> extra ) && !( w >> extra );
    }
}  // namespace

void test_reference_chain( TestRun& t )
{
    const std::string dir = LIGHT_AI_TEST_DATA_DIR;
    std::ifstream fixture( dir + "/ref_evening_raw.csv" );
    CHECK( t, fixture.is_open() );
    std::ostringstream out;
    int frames = 0;
    const Coverage cov = run_clip( fixture, out, frames );
    CHECK( t, frames > 0 );
    CHECK( t, fixture.eof() );  // the whole fixture parsed
    CHECK( t, cov.dt_clamps >= 1 );
    CHECK( t, cov.tent_deaths >= 1 );
    CHECK( t, cov.ghosts >= 1 );
    CHECK( t, cov.evictions >= 1 );
    int contested_frames = 0;
    run_crossing( out, t, contested_frames );
    int weak_frames = 0;
    run_weak( out, t, weak_frames );

    const std::string golden_path = dir + "/ref_evening_golden.txt";
    const char* regen = std::getenv( "LIGHT_AI_WRITE_GOLDEN" );
    if ( regen != nullptr && std::string( regen ) == "1" )
    {
        std::ofstream g( golden_path );
        g << out.str();
        CHECK( t, static_cast<bool>( g ) );
        std::cout << "reference_chain: golden written: " << golden_path << '\n';
        return;
    }

    std::ifstream golden( golden_path );
    CHECK( t, golden.is_open() );
    std::istringstream got( out.str() );
    std::string a;
    std::string b;
    Deviation dev;
    int line = 0;
    int bad = 0;
    while ( true )
    {
        const bool ga = static_cast<bool>( std::getline( got, a ) );
        const bool gb = static_cast<bool>( std::getline( golden, b ) );
        if ( !ga && !gb )
            break;
        ++line;
        if ( ga && gb && same_line( a, b, dev ) )
            continue;
        if ( ++bad <= kMaxReport )
            std::cerr << "reference_chain: golden line " << line << ":\n  got:  " << ( ga ? a : "<end>" )
                      << "\n  want: " << ( gb ? b : "<end>" ) << '\n';
        if ( ga != gb || ( a[0] == 'f' ) != ( b[0] == 'f' ) )
            break;  // the structure diverged: the rest would only repeat it
    }
    CHECK( t, bad == 0 );
    std::cout << "reference_chain: " << frames << " clip frames, " << line << " lines, max |d| " << dev.px << " px, "
              << dev.pxs << " px/s, " << dev.conf << " conf (tolerance " << kTolPx << ", " << kTolPxs << ", " << kTolConf
              << "); coverage: " << cov.dt_clamps << " dt clamps, " << cov.tent_deaths << " tentative deaths, "
              << cov.ghosts << " ghost states, " << cov.evictions << " evictions, " << contested_frames
              << " contested frames, " << weak_frames << " round-2 frames\n";
}
