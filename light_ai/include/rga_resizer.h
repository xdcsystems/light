#pragma once

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
// The destination is letterboxed: orientation-aware uniform fit.
// A landscape source fills the tensor width
// (visible band = rows [geom.top, geom.top + geom.h));
// a portrait source fills the tensor height
// (visible band = columns [geom.left, geom.left + geom.w)).
// Untouched bars keep what was there before (Pipeline memsets them to 128 once at startup).
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

class RgaResizer
{
public:
    RgaResizer();
    // src: YUVSP (w_src x h_src; 420SP by default, see src_fmt below),
    // dst: RGB888 (w_dst x h_dst).
    // aux: optional intermediate dma-buf for the two-pass path when the
    // scale ratio exceeds the RGA3 per-axis limit (1/8); pass -1 if unused.
    // On success *geom receives the band geometry; on failure geom stays
    // untouched (h = 0 marks "no valid band").
    // src_fmt: 0 = YUV420SP (default), nonzero = YUV422SP --
    // the actual decoder output layout (MppDecoder::out_fmt).
    // The MJPEG decoder follows the stream subsampling and ignores
    // the fmt pre-set on its frame; the RGA read side must match.
    bool resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux, LbGeom* geom, int src_fmt = 0 );
};
