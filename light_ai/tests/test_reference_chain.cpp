// Golden test of the time-dependent half of the reference detections (issue #32,
// layer B; reference/README.md). Recorded raw detections of the reference clip
// (tests/data/ref_evening_raw.csv, a slice of reference/ref_evening.csv) go through
// the chain of Pipeline::publish_oldest() + OverlaySink with FIXED timestamps:
//   BoxSmoother::update( raw -> sm ), Tracker::update( raw, ts -> tracks ),
//   fill_gpu_boxes( sm, tracks, tracker, boxes, lb, sx, sy, ts ) at the production
//   geometry (1280x960 into the 640x640 model: band_geom, sx = lb.w / 1280).
// The output (smoothed boxes, every TrackState, every GpuBox) is compared with
// tests/data/ref_evening_golden.txt, written once by the code of fix/audit-p0 @ f3c5a7f.
//
// Compare: line structure, counts, ids, states, classes, colors and alpha exact;
// floats within a tolerance -- positions / sizes in px (kTolPx), velocities in
// px/s (kTolPxs). Why a tolerance: the build is gnu++17, where GCC contracts
// a*b+c into a fused multiply-add (-ffp-contract=fast); aarch64 always has FMA,
// the x86 baseline does not, so the same code differs in the last bits between
// the board and an x86 host. The tolerances sit well above the measured
// spread (tests/data/README) and well below a real change: the kQAccel
// 2000 -> 2100 nudge fails this test.
//
// Timestamps: fixed, kStepUs per frame block; where the fixture's frame numbers
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
            put( out, "s %d %.4f %.4f %.4f %.4f %.4f", d.cls, d.conf, d.x, d.y, d.w, d.h );
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

    // Runs the chain over the fixture, the order of Pipeline::publish_oldest(); returns the frame count.
    int run_chain( std::istream& in, std::ostream& out, Coverage& cov )
    {
        BoxSmoother smoother;
        Tracker tracker;
        DetList raw;
        DetList sm;
        TrajList tracks;
        TrajList prev;
        GpuBoxList boxes;
        const LbGeom lb = band_geom( kSrcW, kSrcH, kModel, kModel );
        const float sx = static_cast<float>( lb.w ) / static_cast<float>( kSrcW );
        const float sy = static_cast<float>( lb.h ) / static_cast<float>( kSrcH );
        int frame = 0;
        int prev_frame = 0;
        int frames = 0;
        int64_t ts = 0;
        int64_t prev_ts = 0;
        while ( next_frame( in, frame, raw ) )
        {
            if ( frames > 0 )
                ts += kStepUs + ( frame != prev_frame + 1 ? kJumpUs : 0 );
            prev_frame = frame;
            smoother.update( raw, sm );
            tracker.update( raw, ts, tracks );
            fill_gpu_boxes( sm, tracks, tracker, boxes, lb, sx, sy, ts );
            if ( frames > 0 )
                cover( prev, tracks, ts - prev_ts, cov );
            prev = tracks;
            prev_ts = ts;
            ++frames;
            write_frame( out, frame, ts, sm, tracks, boxes );
        }
        return frames;
    }

    // Token classes per line kind: i = exact, p = px (kTolPx), v = px/s (kTolPxs).
    const char* token_classes( char kind )
    {
        switch ( kind )
        {
            case 'f':
                return "iiiii";
            case 's':
                return "ippppp";
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
            double& mx = *c == 'p' ? dev.px : dev.pxs;
            mx = std::max( mx, d );
            ok = ok && d <= static_cast<double>( *c == 'p' ? kTolPx : kTolPxs );
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
    Coverage cov;
    const int frames = run_chain( fixture, out, cov );
    CHECK( t, frames > 0 );
    CHECK( t, fixture.eof() );  // the whole fixture parsed
    CHECK( t, cov.dt_clamps >= 1 );
    CHECK( t, cov.tent_deaths >= 1 );
    CHECK( t, cov.ghosts >= 1 );
    CHECK( t, cov.evictions >= 1 );

    const std::string golden_path = dir + "/ref_evening_golden.txt";
    const char* regen = std::getenv( "LIGHT_AI_WRITE_GOLDEN" );
    if ( regen != nullptr && std::string( regen ) == "1" )
    {
        std::ofstream g( golden_path );
        g << out.str();
        CHECK( t, static_cast<bool>( g ) );
        std::cout << "reference_chain: golden written: " << golden_path << " (" << frames << " frames)\n";
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
    std::cout << "reference_chain: " << frames << " frames, " << line << " lines, max |d| " << dev.px << " px, " << dev.pxs
              << " px/s (tolerance " << kTolPx << ", " << kTolPxs << "); coverage: " << cov.dt_clamps << " dt clamps, "
              << cov.tent_deaths << " tentative deaths, " << cov.ghosts << " ghost states, " << cov.evictions
              << " evictions\n";
}
