// BoxFilter / TrackBoxFilters (core/box_filter.hpp, #91 item 4): a standing
// jittering box is still, a step converges without overshoot, a slow walk never
// freezes and never jumps (the BoxEnvelope faults of audit M-3), the dead zone
// does not chatter at its edge, bad input and bad dt hold the output, and a new
// track generation starts fresh.

#include <cmath>
#include <cstdint>

#include "check.hpp"
#include "core/box_filter.hpp"
#include "suites.hpp"

namespace
{
    constexpr float kDt = 0.05f;  // 20 FPS
    const BoxFilterParams kRaw { 0.5f, 0.01f, 1.0f, 0.02f, 2.0f, 0.5f };    // BoxSmoother's set
    const BoxFilterParams kTrack { 1.0f, 0.05f, 1.0f, 0.01f, 2.0f, 0.5f };  // OverlaySink's set

    // Deterministic uniform noise in [-a, a] (an LCG: no <random>, same on every host).
    struct Noise
    {
        uint32_t s = 12345u;
        float next( float a ) noexcept
        {
            s = s * 1664525u + 1013904223u;
            const float u = static_cast<float>( s >> 8 ) / static_cast<float>( 1u << 24 );  // [0, 1)
            return ( 2.0f * u - 1.0f ) * a;
        }
    };

    bool same( const BoxCWH& a, const BoxCWH& b )
    {
        return a.cx == b.cx && a.cy == b.cy && a.w == b.w && a.h == b.h;
    }

    // A standing box with uniform jitter of +-amp px on every axis: after warm-up
    // the output never changes again.
    void still_under_jitter( TestRun& t, const BoxFilterParams& p, float w, float h, float amp )
    {
        Noise n;
        BoxFilter f;
        const BoxCWH truth { 400.0f, 300.0f, w, h };
        f.reset( truth );
        auto jittered = [&n, &truth, amp]() {
            return BoxCWH { truth.cx + n.next( amp ), truth.cy + n.next( amp ), truth.w + n.next( amp ), truth.h + n.next( amp ) };
        };
        for ( int k = 0; k < 60; ++k )
            f.step( jittered(), kDt, p );
        const BoxCWH settled = f.out();
        int moved = 0;
        for ( int k = 0; k < 600; ++k )
        {
            const BoxCWH before = f.out();
            f.step( jittered(), kDt, p );
            moved += same( before, f.out() ) ? 0 : 1;
        }
        CHECK_EQ( t, moved, 0 );
        // ...and the still box sits within one dead zone (+ 1 px of filtered jitter) of the true box.
        const float tc[BoxFilter::kAxes] = { truth.cx, truth.cy, truth.w, truth.h };
        CHECK_NEAR( t, settled.cx, truth.cx, box_filter::dead_zone( 0, tc, p ) + 1.0f );
        CHECK_NEAR( t, settled.cy, truth.cy, box_filter::dead_zone( 1, tc, p ) + 1.0f );
    }

    // A box steps 60 px to the right: converges, never overshoots, never steps back.
    void step_response( TestRun& t, const BoxFilterParams& p )
    {
        BoxFilter f;
        f.reset( BoxCWH { 100.0f, 200.0f, 80.0f, 120.0f } );
        float prev = f.out().cx;
        bool monotone = true;
        bool no_overshoot = true;
        int frames_to_settle = -1;
        for ( int k = 1; k <= 60; ++k )
        {
            f.step( BoxCWH { 160.0f, 200.0f, 80.0f, 120.0f }, kDt, p );
            const float x = f.out().cx;
            monotone = monotone && x >= prev;
            no_overshoot = no_overshoot && x <= 160.0f;
            if ( frames_to_settle < 0 && x >= 160.0f - 2.0f - 0.5f )  // within the dead zone + 0.5 px
                frames_to_settle = k;
            prev = x;
        }
        CHECK( t, monotone );
        CHECK( t, no_overshoot );
        CHECK( t, frames_to_settle > 0 && frames_to_settle <= 30 );  // 1.5 s at 20 FPS
        // The other axes did not move.
        CHECK( t, f.out().cy == 200.0f && f.out().w == 80.0f && f.out().h == 120.0f );
    }

    // A box stands for 30 frames, then walks at spd px/frame. The output starts
    // within a few frames, then moves every frame, never faster than 1.15x the walk
    // (no catch-up burst), and the lag stays bounded.
    void slow_walk( TestRun& t, const BoxFilterParams& p, float spd )
    {
        BoxFilter f;
        f.reset( BoxCWH { 140.0f, 160.0f, 80.0f, 120.0f } );
        for ( int k = 0; k < 30; ++k )
            f.step( BoxCWH { 140.0f, 160.0f, 80.0f, 120.0f }, kDt, p );
        float prev = f.out().cx;
        int first_move = -1;
        int frozen_after = 0;
        float max_step = 0.0f;
        float x_true = 140.0f;
        for ( int k = 1; k <= 80; ++k )
        {
            x_true = 140.0f + spd * static_cast<float>( k );
            f.step( BoxCWH { x_true, 160.0f, 80.0f, 120.0f }, kDt, p );
            const float d = f.out().cx - prev;
            if ( first_move < 0 && d > 0.0f )
                first_move = k;
            if ( first_move > 0 && k > first_move + 2 )
                frozen_after += d > 0.0f ? 0 : 1;
            max_step = std::fmax( max_step, d );
            CHECK( t, d >= 0.0f );
            prev = f.out().cx;
        }
        CHECK( t, first_move > 0 && first_move <= 20 );  // starts within 1 s...
        CHECK( t, static_cast<float>( first_move - 1 ) * spd <= 6.0f );  // ...and within 6 px of walk
        CHECK( t, frozen_after == 0 );
        CHECK( t, max_step <= 1.15f * spd );
        CHECK( t, x_true - f.out().cx <= 9.0f );  // lag: the dead zone (2 px) + the low-pass
    }

    // Hysteresis: a box whose filtered value sits just inside the dead zone of the
    // output stays put; once pushed past the edge, the output follows by exactly the
    // overshoot and stops again -- no back-and-forth.
    void dead_zone_edge( TestRun& t )
    {
        BoxFilter f;
        f.reset( BoxCWH { 100.0f, 100.0f, 50.0f, 50.0f } );  // dz = 2 px on every axis (the floor)
        for ( int k = 0; k < 200; ++k )
            f.step( BoxCWH { 101.9f, 100.0f, 50.0f, 50.0f }, kDt, kRaw );
        CHECK( t, f.out().cx == 100.0f );  // 1.9 px < dz: never moved
        for ( int k = 0; k < 200; ++k )
            f.step( BoxCWH { 103.0f, 100.0f, 50.0f, 50.0f }, kDt, kRaw );
        CHECK_NEAR( t, f.out().cx, 101.0f, 1e-3f );  // dragged to 103 - dz
        // Back within the zone on the other side: the output does not move back.
        float prev = f.out().cx;
        int reversals = 0;
        for ( int k = 0; k < 200; ++k )
        {
            f.step( BoxCWH { k % 2 == 0 ? 99.5f : 102.5f, 100.0f, 50.0f, 50.0f }, kDt, kRaw );
            reversals += f.out().cx < prev ? 1 : 0;
            prev = f.out().cx;
        }
        CHECK( t, reversals == 0 );
    }

    void bad_input( TestRun& t )
    {
        BoxFilter f;
        CHECK( t, !f.primed() );
        f.step( BoxCWH { std::nanf( "" ), 1.0f, 2.0f, 3.0f }, kDt, kRaw );
        CHECK( t, !f.primed() );  // a NaN box never primes
        f.step( BoxCWH { 10.0f, 20.0f, 30.0f, 40.0f }, kDt, kRaw );
        CHECK( t, f.primed() && same( f.out(), BoxCWH { 10.0f, 20.0f, 30.0f, 40.0f } ) );  // first box: as is
        const BoxCWH o = f.out();
        f.step( BoxCWH { 50.0f, std::nanf( "" ), 30.0f, 40.0f }, kDt, kRaw );
        f.step( BoxCWH { 50.0f, 20.0f, INFINITY, 40.0f }, kDt, kRaw );
        f.step( BoxCWH { 50.0f, 20.0f, 30.0f, 0.0f }, kDt, kRaw );
        f.step( BoxCWH { 50.0f, 20.0f, -1.0f, 40.0f }, kDt, kRaw );
        CHECK( t, same( f.out(), o ) );  // unusable boxes leave the output as it was
        f.step( BoxCWH { 50.0f, 20.0f, 30.0f, 40.0f }, 0.0f, kRaw );
        f.step( BoxCWH { 50.0f, 20.0f, 30.0f, 40.0f }, -0.04f, kRaw );
        f.step( BoxCWH { 50.0f, 20.0f, 30.0f, 40.0f }, std::nanf( "" ), kRaw );
        CHECK( t, same( f.out(), o ) );  // dt <= 0 or NaN: held
        f.step( BoxCWH { 50.0f, 20.0f, 30.0f, 40.0f }, 0.6f, kRaw );
        CHECK( t, same( f.out(), BoxCWH { 50.0f, 20.0f, 30.0f, 40.0f } ) );  // a gap above max_dt restarts
        f.step( BoxCWH { 60.0f, 20.0f, 30.0f, 40.0f }, kDt, kRaw );
        CHECK( t, std::isfinite( f.out().cx ) && f.out().cx > 50.0f && f.out().cx < 60.0f );
    }

    TrackState track( int id, uint32_t gen, TrackLife st, float cx )
    {
        TrackState s {};
        s.id = id;
        s.gen = gen;
        s.state = st;
        s.cx = cx;
        s.cy = 300.0f;
        s.bw = 100.0f;
        s.bh = 200.0f;
        return s;
    }

    void track_filters( TestRun& t )
    {
        TrackBoxFilters tf;
        int64_t ts = 0;
        TrajList l;
        l.push( track( 1, 7, TRACK_CONF, 400.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 400.0f, 1e-6f );  // a new track starts at its box
        for ( int k = 0; k < 3; ++k )
        {
            ts += 50000;
            l.clear();
            l.push( track( 1, 7, TRACK_CONF, 440.0f ) );
            tf.apply( l, ts, kTrack );
        }
        const float filtered = l.d[0].cx;
        CHECK( t, filtered > 400.0f && filtered < 440.0f );  // filtered, not the raw box
        // The same slot id with a new gen is another person: fresh, at its raw box.
        ts += 50000;
        l.clear();
        l.push( track( 1, 8, TRACK_CONF, 440.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 440.0f, 1e-6f );
        // A DEAD ghost passes through untouched and drops its filter.
        ts += 50000;
        l.clear();
        l.push( track( 1, 8, TRACK_DEAD, 900.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 900.0f, 1e-6f );
        ts += 50000;
        l.clear();
        l.push( track( 1, 8, TRACK_CONF, 500.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 500.0f, 1e-6f );  // no filter survived the DEAD frame
        // clear(): every track starts fresh.
        tf.clear();
        ts += 50000;
        l.clear();
        l.push( track( 1, 8, TRACK_CONF, 520.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 520.0f, 1e-6f );
        // A frame with a non-increasing timestamp holds the box (dt <= 0).
        l.clear();
        l.push( track( 1, 8, TRACK_CONF, 560.0f ) );
        tf.apply( l, ts, kTrack );
        CHECK_NEAR( t, l.d[0].cx, 520.0f, 1e-6f );
    }
}  // namespace

void test_box_filter( TestRun& t )
{
    still_under_jitter( t, kRaw, 200.0f, 300.0f, 1.0f );  // a chair: dz 4 / 6 px
    still_under_jitter( t, kRaw, 200.0f, 300.0f, 3.0f );
    still_under_jitter( t, kRaw, 40.0f, 60.0f, 1.5f );    // a bottle: dz 2 px (the floor)
    still_under_jitter( t, kTrack, 330.0f, 600.0f, 1.0f );  // a person: dz 3.3 / 6 px
    still_under_jitter( t, kTrack, 330.0f, 600.0f, 2.0f );
    step_response( t, kRaw );
    step_response( t, kTrack );
    const float speeds[5] = { 0.2f, 0.5f, 1.0f, 2.0f, 5.0f };
    for ( const float spd : speeds )
    {
        slow_walk( t, kRaw, spd );
        slow_walk( t, kTrack, spd );
    }
    dead_zone_edge( t );
    bad_input( t );
    track_filters( t );
}
