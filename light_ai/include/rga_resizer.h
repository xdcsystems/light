#pragma once

#include "core/geometry.hpp"  // LbGeom, band_geom

class Log;

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
// The destination is letterboxed: orientation-aware uniform fit.
// A landscape source fills the tensor width
// (visible band = rows [geom.top, geom.top + geom.h));
// a portrait source fills the tensor height
// (visible band = columns [geom.left, geom.left + geom.w)).
// Untouched bars keep what was there before (Pipeline memsets them to 128 once at startup).
class RgaResizer
{
public:
    // Pins the jobs to RGA3 core0; failures go to log (`err` stream).
    explicit RgaResizer( const Log& log );
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

private:
    // Pass 1 of the two-pass path: src (src_fmt_rk) into aux (NV12, w_aux x h_aux).
    bool downscale_to_aux( int src_fd, int w_src, int h_src, int src_fmt_rk, int aux_fd, int w_aux, int h_aux ) const;

private:
    const Log& log_;
};
