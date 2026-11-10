#pragma once

#include <algorithm>
#include <cstddef>

// The single IoU implementation for all box consumers (Tracker
// association/spawn, BoxSmoother matching, YOLOv5 NMS).
// Two box representations exist in the project, and both are kept:
//   - xywh (top-left corner + size): Detection, BoxTrack;
//   - corners (x1,y1,x2,y2): the NMS candidate Box in yolo_postprocess.
// The degenerate-union guards differ deliberately:
//   - box_iou returns 0.0f on an empty intersection (tracker/smooth
//     contract, unchanged);
//   - box_iou_corners carries the +1e-6f epsilon: two degenerate
//     zero-area candidates must not produce a division by zero in NMS
//     (the old yolo_postprocess guard, moved here verbatim).

inline float box_iou( float ax, float ay, float aw, float ah,
                      float bx, float by, float bw, float bh )
{
    const float x0 = ax > bx ? ax : bx;
    const float y0 = ay > by ? ay : by;
    const float x1 = ( ax + aw ) < ( bx + bw ) ? ( ax + aw ) : ( bx + bw );
    const float y1 = ( ay + ah ) < ( by + bh ) ? ( ay + ah ) : ( by + bh );
    const float iw = x1 - x0;
    const float ih = y1 - y0;
    if ( iw <= 0.0f || ih <= 0.0f )
        return 0.0f;
    const float inter = iw * ih;
    const float uni = aw * ah + bw * bh - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

inline float box_iou_corners( float ax1, float ay1, float ax2, float ay2,
                              float bx1, float by1, float bx2, float by2 )
{
    const float ix1 = ax1 > bx1 ? ax1 : bx1;
    const float iy1 = ay1 > by1 ? ay1 : by1;
    const float ix2 = ax2 < bx2 ? ax2 : bx2;
    const float iy2 = ay2 < by2 ? ay2 : by2;
    const float iw = ix2 > ix1 ? ix2 - ix1 : 0.0f;
    const float ih = iy2 > iy1 ? iy2 - iy1 : 0.0f;
    const float inter = iw * ih;
    const float area_a = ( ax2 - ax1 ) * ( ay2 - ay1 );
    const float area_b = ( bx2 - bx1 ) * ( by2 - by1 );
    // +1e-6f: both degenerate zero-area boxes must not crash the NMS.
    return inter / ( area_a + area_b - inter + 1e-6f );
}

// The one greedy bipartite IoU matching for all consumers (was
// duplicated in BoxSmoother::update() and Tracker::associate()).
// Deterministic order: IoU descending, then det index, then track
// index -- identical to both former policies (BoxSmoother's
// full-order comparator; Tracker's stable insertion sort over its
// det-major collection loop produced the same tie-break).
// Both callers store IouPair (BoxSmoother's nested Pair is now an
// alias). Accepted pairs are compacted to the front; returns their count.
// Per-match work (smoothing, measure, promotion) stays with callers:
// det_trk holds -1 = unmatched, else the matched track index.
struct IouPair
{
    int det;
    int trk;
    float iou;
};

inline size_t greedy_iou_match( IouPair* pairs, size_t np, int* det_trk,
                                unsigned char* trk_taken )
{
    std::sort( pairs, pairs + np, []( const IouPair& a, const IouPair& b ) {
        return a.iou != b.iou ? a.iou > b.iou
                              : ( a.det != b.det ? a.det < b.det : a.trk < b.trk );
    } );
    size_t na = 0;
    for ( size_t p = 0; p < np; ++p )
    {
        const IouPair pr = pairs[p];
        if ( det_trk[pr.det] >= 0 || trk_taken[pr.trk] )
            continue;
        det_trk[pr.det] = pr.trk;
        trk_taken[pr.trk] = 1;
        pairs[na++] = pr;  // compaction cannot overtake: na <= p
    }
    return na;
}
