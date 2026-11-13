// Letterbox geometry: the domain type shared by the resize, postprocess,
// overlay and encoder layers. Pure integer math, no SDK dependencies
// (moved out of rga_resizer.h so consumers stop including the RGA layer).

#pragma once

// Letterbox geometry of one resize -- for the detection mapping T:
// scene = (det - band origin) * src / band size, axial (see yolo_postprocess):
// x_scene = (x_det - left) * w_src / w; y_scene = (y_det - top) * h_src / h;
// a detection outside [left, left + w) x [top, top + h) sits on a bar.
// Returned BY VALUE per call: no shared mutable state --
// with --pool 3 the old member-based lb_top_/lb_h_ was written and read across threads.
struct LbGeom
{
    int left = 0;
    int w = 0;   // portrait: the visible band is a vertical strip [left, left + w)
    int top = 0;
    int h = 0;   // landscape: the visible band is a horizontal strip [top, top + h)
};

// Single-owner letterbox geometry: one formula for the RGA crop,
// the encoder crop and the postprocess scale. Integer truncation.
LbGeom band_geom( int w_src, int h_src, int w_dst, int h_dst );
