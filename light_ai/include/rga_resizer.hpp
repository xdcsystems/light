#pragma once

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
// The destination is letterboxed: uniform scale by width (s = w_dst/w_src),
// the visible band goes into rows [geom.top, geom.top + geom.h) of the
// destination; the remaining rows keep what was there before (Pipeline
// memsets them to 128 once at startup).
// Letterbox geometry of one resize — for the detection mapping T:
// x_scene = x_det * w_src / w_dst; y_scene = (y_det - top) * w_src / w_dst;
// a detection with y_det < top or >= top + h sits on a bar.
// Returned BY VALUE per call (P1): no shared mutable state — with --pool 3
// the old member-based lb_top_/lb_h_ was written and read across threads.
struct LbGeom
{
    int top = 0;
    int h = 0;
};

class RgaResizer
{
public:
    RgaResizer();
    // Single-owner letterbox geometry: one formula for the RGA crop,
    // the encoder crop and the postprocess scale. Integer truncation.
    static LbGeom band_geom( int w_src, int h_src, int w_dst, int h_dst );
    // src: YUV422SP (w_src x h_src), dst: RGB888 (w_dst x h_dst).
    // aux: optional intermediate dma-buf for the two-pass path when the
    // scale ratio exceeds the RGA3 per-axis limit (1/8); pass -1 if unused.
    // On success *geom receives the band geometry; on failure geom stays
    // untouched (h = 0 marks "no valid band").
    bool resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux, LbGeom* geom );
};
