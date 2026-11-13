#include <array>
#include <cstdint>

#include "check.hpp"
#include "suites.hpp"
#include "tracker.hpp"

namespace
{
    enum : int64_t { kFrameUs = 40000 };  // 25 fps

    // Person detection from its center and size (Detection is top-left + size).
    Detection person( float cx, float cy, float w, float h )
    {
        return Detection { cx - w * 0.5f, cy - h * 0.5f, w, h, 0, 0.9f };
    }

    DetList one( const Detection& d )
    {
        DetList l;
        l.push( d );
        return l;
    }

    // Feeds the same standing person for `frames` frames starting at `ts`;
    // returns the timestamp of the next frame.
    int64_t stand( Tracker& trk, TrajList& out, int frames, int64_t ts )
    {
        for ( int i = 0; i < frames; ++i, ts += kFrameUs )
            trk.update( one( person( 500, 400, 100, 200 ) ), ts, out );
        return ts;
    }
}  // namespace

void test_tracker( TestRun& t )
{
    // Lifecycle: tentative on the first sighting, confirmed on the 3rd match.
    {
        Tracker trk;
        TrajList out;
        trk.update( one( person( 500, 400, 100, 200 ) ), 0, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_TENT );
        CHECK_NEAR( t, out.d[0].cx, 500.0f, 1e-3f );
        CHECK_NEAR( t, out.d[0].cy, 400.0f, 1e-3f );
        CHECK( t, out.d[0].n_pts == 1 );  // spawn seeds the trail
        trk.update( one( person( 500, 400, 100, 200 ) ), kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_TENT );
        trk.update( one( person( 500, 400, 100, 200 ) ), 2 * kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].bw, 100.0f, 1e-2f );
        CHECK_NEAR( t, out.d[0].bh, 200.0f, 1e-2f );
    }
    // Preallocated buffer contract: out is cleared at entry.
    {
        Tracker trk;
        TrajList out;
        out.push( TrackState {} );
        out.push( TrackState {} );
        trk.update( DetList {}, 0, out );
        CHECK( t, out.n == 0 );
    }
    // Only persons (class 0) are tracked.
    {
        Tracker trk;
        TrajList out;
        Detection chair = person( 500, 400, 100, 200 );
        chair.cls = 56;
        trk.update( one( chair ), 0, out );
        CHECK( t, out.n == 0 );
    }
    // A confirmed track coasts through 4 missed frames and dies on the 5th;
    // the ghost keeps its trail until the points age out (4 s), then the slot is freed.
    {
        Tracker trk;
        TrajList out;
        int64_t ts = stand( trk, out, 5, 0 );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF );
        for ( int miss = 1; miss <= 4; ++miss, ts += kFrameUs )
        {
            trk.update( DetList {}, ts, out );
            CHECK( t, out.n == 1 && out.d[0].state == TRACK_CONF );
        }
        trk.update( DetList {}, ts, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_DEAD && out.d[0].id == 1 );
        trk.update( DetList {}, ts + 5000000, out );
        CHECK( t, out.n == 0 );
    }
    // A brief dropout does not change the identity.
    {
        Tracker trk;
        TrajList out;
        int64_t ts = stand( trk, out, 5, 0 );
        trk.update( DetList {}, ts, out );
        trk.update( DetList {}, ts + kFrameUs, out );
        stand( trk, out, 1, ts + 2 * kFrameUs );
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
    }
    // A tentative track dies by wall clock: alive at exactly 1 s without a match, dead after.
    {
        Tracker trk;
        TrajList out;
        trk.update( one( person( 500, 400, 100, 200 ) ), 0, out );
        trk.update( DetList {}, 1000000, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_TENT );
        trk.update( DetList {}, 1040000, out );
        CHECK( t, out.n == 1 && out.d[0].state == TRACK_DEAD );
    }
    // Constant-velocity walk (100 px/s): one stable id, the filter follows
    // position and velocity, the trail grows oldest-first.
    {
        Tracker trk;
        TrajList out;
        float cx = 200.0f;
        int64_t ts = 0;
        for ( int i = 0; i < 100; ++i, ts += kFrameUs, cx += 4.0f )
            trk.update( one( person( cx, 400, 100, 200 ) ), ts, out );
        cx -= 4.0f;  // the last fed position
        CHECK( t, out.n == 1 && out.d[0].id == 1 && out.d[0].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].cx, cx, 6.0f );
        CHECK_NEAR( t, out.d[0].cy, 400.0f, 1.0f );
        CHECK_NEAR( t, out.d[0].vx, 100.0f, 10.0f );
        CHECK_NEAR( t, out.d[0].vy, 0.0f, 5.0f );
        CHECK( t, out.d[0].n_pts > 10 );

        std::array<TrajPoint, 20> pts {};
        const int n = trk.recent_points( 1, 1, pts.data(), 20 );
        CHECK( t, n == 20 );  // capped by dst_cap
        bool ordered = true;
        for ( int k = 1; k < n; ++k )
            ordered = ordered && pts[k].ts_us > pts[k - 1].ts_us && pts[k].x > pts[k - 1].x;
        CHECK( t, ordered );
        CHECK_NEAR( t, pts[n - 1].x, cx, 20.0f );  // the newest point is always included

        std::array<TrajPoint, 20> sparse {};
        const int ns = trk.recent_points( 1, 4, sparse.data(), 20 );
        CHECK( t, ns == ( out.d[0].n_pts + 3 ) / 4 || ns == 20 );
        CHECK( t, sparse[ns - 1].ts_us == pts[n - 1].ts_us );  // stride keeps the newest

        CHECK( t, trk.recent_points( 99, 1, pts.data(), 20 ) == 0 );  // unknown id
        CHECK( t, trk.recent_points( 1, 0, pts.data(), 20 ) == 0 );   // bad stride
        CHECK( t, trk.recent_points( 1, 1, nullptr, 20 ) == 0 );
        CHECK( t, trk.recent_points( 1, 1, pts.data(), 0 ) == 0 );
    }
    // Two persons far apart keep two distinct stable ids.
    {
        Tracker trk;
        TrajList out;
        for ( int i = 0; i < 10; ++i )
        {
            DetList dets;
            dets.push( person( 300, 400, 100, 200 ) );
            dets.push( person( 900, 400, 100, 200 ) );
            trk.update( dets, i * kFrameUs, out );
        }
        CHECK( t, out.n == 2 );
        CHECK( t, out.d[0].id == 1 && out.d[1].id == 2 );
        CHECK( t, out.d[0].state == TRACK_CONF && out.d[1].state == TRACK_CONF );
        CHECK_NEAR( t, out.d[0].cx, 300.0f, 1.0f );
        CHECK_NEAR( t, out.d[1].cx, 900.0f, 1.0f );
    }
    // Spawn dedupe: an unmatched detection overlapping a live track spawns nothing.
    {
        Tracker trk;
        TrajList out;
        DetList dets;
        dets.push( person( 500, 400, 100, 200 ) );
        dets.push( person( 505, 400, 100, 200 ) );
        trk.update( dets, 0, out );
        CHECK( t, out.n == 1 );
    }
    // Size-consistency gate: a box squeezed below 40% of the track size does not
    // match; it overlaps the track, so it is deduped instead of spawning.
    {
        Tracker trk;
        TrajList out;
        const int64_t ts = stand( trk, out, 5, 0 );
        trk.update( one( person( 500, 400, 30, 200 ) ), ts, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 );
        CHECK_NEAR( t, out.d[0].bw, 100.0f, 1e-2f );  // the squeezed box never reached the filter
    }
    // Capacity: 8 track slots; the 9th person is dropped.
    {
        Tracker trk;
        TrajList out;
        DetList dets;
        for ( int i = 0; i < 9; ++i )
            dets.push( person( 100.0f + 300.0f * static_cast<float>( i ), 400, 100, 200 ) );
        trk.update( dets, 0, out );
        CHECK( t, out.n == 8 );
        CHECK( t, out.d[7].id == 8 );
    }
    // reset() restarts the id sequence.
    {
        Tracker trk;
        TrajList out;
        stand( trk, out, 3, 0 );
        trk.reset();
        trk.update( DetList {}, 0, out );
        CHECK( t, out.n == 0 );
        trk.update( one( person( 500, 400, 100, 200 ) ), kFrameUs, out );
        CHECK( t, out.n == 1 && out.d[0].id == 1 );
    }
    CHECK( t, Tracker {}.trail_max_age_us() == 4000000 );
}
