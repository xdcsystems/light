#include <cmath>

#include "check.hpp"
#include "core/auto_orient.hpp"
#include "suites.hpp"

void test_auto_orient( TestRun& t )
{
    // 1. Score favors upright vertical detections over rotated horizontal ones.
    {
        DetList upright {};
        upright.push( Detection { 100.0f, 50.0f, 120.0f, 340.0f, 0, 0.85f } );  // upright person
        upright.push( Detection { 400.0f, 180.0f, 80.0f, 160.0f, 56, 0.70f } );  // upright chair

        DetList sideways {};
        sideways.push( Detection { 100.0f, 50.0f, 320.0f, 110.0f, 0, 0.42f } );  // sideways person

        const float sc_up = auto_orient::score_detections( upright );
        const float sc_side = auto_orient::score_detections( sideways );
        CHECK( t, sc_up > 2.0f && sc_up > sc_side * 3.0f );
        CHECK_NEAR( t, auto_orient::score_detections( DetList {} ), 0.0f, 1e-6f );
    }

    // 2. Explicit --src-rot locks immediately and ignores EXIF/probes.
    {
        AutoOrient ao;
        ao.reset( false, 180, true );
        CHECK( t, !ao.enabled() && ao.locked() && ao.active_rot() == 180 );
        ao.set_exif_hint( 270 );
        CHECK( t, ao.active_rot() == 180 );
        CHECK( t, ao.next_action( 0 ) == AutoOrient::Action::None );
    }

    // 3. Phone-on-side scenario: 270 deg strongly outperforms 0/90/180 and locks on round 1.
    {
        AutoOrient ao;
        ao.reset( true, 0, true );
        CHECK( t, ao.next_action( 0 ) == AutoOrient::Action::ProbeAll );
        const float scores[4] = { 0.0f, 0.36f, 0.45f, 3.80f };
        const AutoOrient::Result r = ao.commit_all4( scores );
        CHECK( t, r.changed && r.locked && r.prev_rot == 0 && r.rot_deg == 270 );
        CHECK( t, ao.active_rot() == 270 && ao.next_action( 1 ) == AutoOrient::Action::None );
    }

    // 4. Moderate signal requires two agreeing rounds before locking.
    {
        AutoOrient ao;
        ao.reset( true, 0, false );
        const float mod[4] = { 0.0f, 0.70f, 0.0f, 0.0f };
        const AutoOrient::Result r1 = ao.commit_all4( mod );
        CHECK( t, r1.changed && !r1.locked && r1.rot_deg == 90 );
        const AutoOrient::Result r2 = ao.commit_all4( mod );
        CHECK( t, !r2.changed && r2.locked && r2.rot_deg == 90 );
    }

    // 5. Empty room enters Watch mode after burst rounds, then wakes on single-angle detection.
    {
        AutoOrient ao;
        ao.reset( true, 0, true );
        const float empty[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for ( int i = 0; i < 3; ++i )
            static_cast<void>( ao.commit_all4( empty ) );
        CHECK( t, !ao.locked() && ao.active_rot() == 0 );
        CHECK( t, ao.next_action( 5 ) == AutoOrient::Action::None );
        CHECK( t, ao.next_action( 16 ) == AutoOrient::Action::ProbeSingle );
        const int probe_deg = ao.watch_angle();
        CHECK( t, probe_deg == 90 );
        CHECK( t, ao.observe_single( probe_deg, 0.80f ) );
        CHECK( t, ao.next_action( 17 ) == AutoOrient::Action::ProbeAll );
    }

    // 6. Oriented dimensions helper swaps on 90/270 only.
    {
        int w = 0;
        int h = 0;
        auto_orient::oriented_dims( 720, 1280, 270, w, h );
        CHECK( t, w == 1280 && h == 720 );
        auto_orient::oriented_dims( 1280, 720, 180, w, h );
        CHECK( t, w == 1280 && h == 720 );
    }
}
