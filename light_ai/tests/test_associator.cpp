// Associator: candidates per round, the size gate of each round, the IoU
// gates, the skip rules and the best-first order. Track boxes 50 x 100.

#include <cmath>

#include "core/associator.hpp"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    // A track box from its top-left corner (the association works on centers).
    TrackBox box( float x, float y, TrackLife life )
    {
        return TrackBox { x + 25.0f, y + 50.0f, 50.0f, 100.0f, life };
    }

    DetList dets_of( std::initializer_list<Detection> l )
    {
        DetList d;
        for ( const Detection& e : l )
            d.push( e );
        return d;
    }

    struct Round
    {
        Associator::DetTrack det_trk {};
        Associator::TrackTaken taken {};
        Round() { det_trk.fill( -1 ); }
    };

    // The same box: matched in round 1; a non-person det and the free / dead
    // tracks never take part.
    void test_basic( TestRun& t )
    {
        Associator a;
        Associator::TrackBoxes tracks {};
        tracks[0] = box( 100, 100, TRACK_CONF );
        tracks[1] = box( 100, 100, TRACK_DEAD );
        tracks[2] = box( 100, 100, TRACK_FREE );
        const DetList dets = dets_of( { { 100, 100, 50, 100, 56, 0.9f }, { 100, 100, 50, 100, 0, 0.9f } } );
        Round r;
        CHECK( t, a.match( 0, dets, tracks, r.det_trk, r.taken ) == 1 );
        CHECK( t, r.det_trk[0] == -1 && r.det_trk[1] == 0 );
        CHECK( t, r.taken[0] == 1 && r.taken[1] == 0 && r.taken[2] == 0 );
        CHECK( t, a.pair( 0 ).det == 1 && a.pair( 0 ).trk == 0 && a.pair( 0 ).iou == 1.0f );

        // Round 2 after it: nothing left to pair.
        CHECK( t, a.match( 1, dets, tracks, r.det_trk, r.taken ) == 0 );
    }

    // A det 2.5x the track size, same center: above kSizeMax (200 %) for
    // round 1, within kSizeMaxWeak (300 %) for round 2, IoU 5000 / 31250 =
    // 0.16 >= kIoUGateWeak. A CONF track takes it in round 2, a TENT never.
    void test_size_gate( TestRun& t )
    {
        const Detection big { 62.5f, 25.0f, 125.0f, 250.0f, 0, 0.5f };  // center (125, 150)
        Associator a;
        Associator::TrackBoxes conf {};
        conf[0] = box( 100, 100, TRACK_CONF );
        Round r;
        CHECK( t, a.match( 0, dets_of( { big } ), conf, r.det_trk, r.taken ) == 0 );
        CHECK( t, a.match( 1, dets_of( { big } ), conf, r.det_trk, r.taken ) == 1 );
        CHECK( t, r.det_trk[0] == 0 );

        Associator::TrackBoxes tent {};
        tent[0] = box( 100, 100, TRACK_TENT );
        Round rt;
        CHECK( t, a.match( 0, dets_of( { big } ), tent, rt.det_trk, rt.taken ) == 0 );
        CHECK( t, a.match( 1, dets_of( { big } ), tent, rt.det_trk, rt.taken ) == 0 );

        // A TENT is a round-1 candidate: the same-size det matches it there.
        Round rs;
        CHECK( t, a.match( 0, dets_of( { { 100, 100, 50, 100, 0, 0.9f } } ), tent, rs.det_trk, rs.taken ) == 1 );

        // Below kSizeMin (40 %): never, in either round.
        const Detection tiny { 115.0f, 135.0f, 15.0f, 30.0f, 0, 0.9f };
        Round rn;
        CHECK( t, a.match( 0, dets_of( { tiny } ), conf, rn.det_trk, rn.taken ) == 0 );
        CHECK( t, a.match( 1, dets_of( { tiny } ), conf, rn.det_trk, rn.taken ) == 0 );

        // A track without an extent is not a candidate.
        Associator::TrackBoxes flat {};
        flat[0] = TrackBox { 125.0f, 150.0f, 0.0f, 100.0f, TRACK_CONF };
        Round rf;
        CHECK( t, a.match( 0, dets_of( { { 100, 100, 50, 100, 0, 0.9f } } ), flat, rf.det_trk, rf.taken ) == 0 );
    }

    // IoU gates: 0.25 fails round 1 (0.30), passes round 2 (0.15).
    void test_iou_gate( TestRun& t )
    {
        Associator a;
        Associator::TrackBoxes tracks {};
        tracks[0] = box( 0, 0, TRACK_CONF );
        const DetList dets = dets_of( { { 30, 0, 50, 100, 0, 0.9f } } );  // 20 / 80 = 0.25
        Round r;
        CHECK( t, a.match( 0, dets, tracks, r.det_trk, r.taken ) == 0 );
        CHECK( t, a.match( 1, dets, tracks, r.det_trk, r.taken ) == 1 );
        CHECK_NEAR( t, a.pair( 0 ).iou, 0.25f, 1e-6f );
    }

    // Best first: det 1 / track B (0.923) before det 0 / track A (0.818); the
    // weaker det 0 / track B (0.333) loses its track. Pre-matched dets and
    // pre-taken tracks are skipped.
    void test_order_and_skips( TestRun& t )
    {
        Associator a;
        Associator::TrackBoxes tracks {};
        tracks[0] = box( 0, 0, TRACK_CONF );   // A: x 0..50
        tracks[1] = box( 30, 0, TRACK_CONF );  // B: x 30..80
        const DetList dets = dets_of( { { 5, 0, 50, 100, 0, 0.9f }, { 28, 0, 50, 100, 0, 0.9f } } );
        Round r;
        CHECK( t, a.match( 0, dets, tracks, r.det_trk, r.taken ) == 2 );
        CHECK( t, a.pair( 0 ).det == 1 && a.pair( 0 ).trk == 1 );
        CHECK( t, a.pair( 1 ).det == 0 && a.pair( 1 ).trk == 0 );
        CHECK( t, r.det_trk[0] == 0 && r.det_trk[1] == 1 );

        Round pre;
        pre.det_trk[1] = 5;  // det 1 matched elsewhere
        pre.taken[0] = 1;    // track A taken elsewhere
        CHECK( t, a.match( 0, dets, tracks, pre.det_trk, pre.taken ) == 1 );
        CHECK( t, a.pair( 0 ).det == 0 && a.pair( 0 ).trk == 1 );
    }

    // Defensive clamp if dets.n exceeds DetList::cap.
    void test_det_cap_clamp( TestRun& t )
    {
        Associator a;
        Associator::TrackBoxes tracks {};
        tracks[0] = box( 100, 100, TRACK_CONF );
        DetList dets;
        dets.d[0] = { 100, 100, 50, 100, 0, 0.9f };
        dets.n = DetList::cap + 5;
        Round r;
        CHECK( t, a.match( 0, dets, tracks, r.det_trk, r.taken ) == 1 );
        CHECK( t, r.det_trk[0] == 0 );
    }

    // M-14: a NaN or zero-size person det is never paired, not even with a
    // track on the same spot.
    void test_unusable( TestRun& t )
    {
        Associator a;
        Associator::TrackBoxes tracks {};
        tracks[0] = box( 100, 100, TRACK_CONF );
        tracks[1] = box( 300, 100, TRACK_CONF );
        const Detection nan_det { std::nanf( "" ), 100, 50, 100, 0, 0.9f };
        const Detection zero_det { 300, 100, 0, 100, 0, 0.9f };
        const DetList dets = dets_of( { nan_det, zero_det } );
        for ( int round = 0; round < 2; ++round )
        {
            Round r;
            CHECK( t, a.match( round, dets, tracks, r.det_trk, r.taken ) == 0 );
            CHECK( t, r.det_trk[0] == -1 && r.det_trk[1] == -1 );
        }
    }
}  // namespace

void test_associator( TestRun& t )
{
    test_unusable( t );
    test_basic( t );
    test_size_gate( t );
    test_iou_gate( t );
    test_order_and_skips( t );
    test_det_cap_clamp( t );
}
