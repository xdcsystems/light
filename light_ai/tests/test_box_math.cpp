#include <array>

#include "box_math.hpp"
#include "check.hpp"
#include "suites.hpp"

void test_box_math( TestRun& t )
{
    // box_iou (xywh): identical, disjoint, touching, half overlap, degenerate.
    CHECK_NEAR( t, box_iou( 0, 0, 10, 10, 0, 0, 10, 10 ), 1.0f, 1e-6f );
    CHECK( t, box_iou( 0, 0, 10, 10, 20, 20, 10, 10 ) == 0.0f );
    CHECK( t, box_iou( 0, 0, 10, 10, 10, 0, 10, 10 ) == 0.0f );
    CHECK_NEAR( t, box_iou( 0, 0, 10, 10, 5, 0, 10, 10 ), 1.0f / 3.0f, 1e-6f );
    CHECK( t, box_iou( 0, 0, 0, 0, 0, 0, 0, 0 ) == 0.0f );

    // box_iou_corners: same geometry, plus the epsilon guard on zero-area boxes.
    CHECK_NEAR( t, box_iou_corners( 0, 0, 10, 10, 5, 0, 15, 10 ), 1.0f / 3.0f, 1e-5f );
    CHECK( t, box_iou_corners( 0, 0, 10, 10, 20, 20, 30, 30 ) == 0.0f );
    const float degenerate = box_iou_corners( 3, 3, 3, 3, 3, 3, 3, 3 );
    CHECK( t, degenerate == degenerate && degenerate == 0.0f );  // no NaN, no division by zero

    // greedy_iou_match: best IoU first; each det and each track is used once;
    // accepted pairs are compacted to the front.
    std::array<IouPair, 4> pairs { { { 0, 0, 0.5f }, { 0, 1, 0.9f }, { 1, 1, 0.8f }, { 1, 0, 0.4f } } };
    std::array<int, 2> det_trk { { -1, -1 } };
    std::array<unsigned char, 2> trk_taken {};
    const size_t na = greedy_iou_match( pairs.data(), pairs.size(), det_trk.data(), trk_taken.data() );
    CHECK( t, na == 2 );
    CHECK( t, det_trk[0] == 1 && det_trk[1] == 0 );
    CHECK( t, trk_taken[0] == 1 && trk_taken[1] == 1 );
    CHECK( t, pairs[0].det == 0 && pairs[0].trk == 1 && pairs[1].det == 1 && pairs[1].trk == 0 );

    // Tie-break on equal IoU: det index ascending, then track index.
    std::array<IouPair, 2> ties { { { 1, 0, 0.7f }, { 0, 0, 0.7f } } };
    std::array<int, 2> tie_det { { -1, -1 } };
    std::array<unsigned char, 1> tie_trk {};
    CHECK( t, greedy_iou_match( ties.data(), ties.size(), tie_det.data(), tie_trk.data() ) == 1 );
    CHECK( t, tie_det[0] == 0 && tie_det[1] == -1 );
}
