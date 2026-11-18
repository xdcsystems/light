#pragma once

#include "core/geometry.hpp"  // LbGeom, band_geom
#include "core/status.hpp"

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
    // src_w_stride / src_h_stride: the source strides; 0 = the 16-aligned
    // w_src / h_src (how MppDecoder allocates its frames).
    // aux (two-pass) is NV12 with 16-aligned strides: the caller allocates at
    // least align16(w_aux) * align16(h_aux) * 3 / 2 bytes (SourceChannel: x2).
    // The MJPEG decoder follows the stream subsampling and ignores
    // the fmt pre-set on its frame; the RGA read side must match.
    // Ok: *geom receives the band geometry. A failure Status carries the
    // cause (aspect mismatch, no aux for the two-pass path, an im2d error
    // with the raw IM_STATUS in sys()); geom stays untouched then.
    Status resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux,
                   LbGeom* geom, int src_fmt = 0, int rot_deg = 0, int src_w_stride = 0, int src_h_stride = 0 );
    // Crops RGB888 sub-rect from model tensor -> dst RGB888 Re-ID tensor: routes >= 68x68
    // crops to RGA3 (IOMMU) and < 68 px crops to RGA2 (< 4G DMA32). Enforces 1/8x .. 8x scale bounds.
    // src / dst are tight (stride = width). A > 8x downscale needs aux: an RGB888
    // w_aux x h_aux intermediate with 16-aligned strides (RGA rejects an unaligned
    // RGB888 width stride), so the caller allocates at least
    // align16(w_aux) * align16(h_aux) * 3 bytes; without aux that case fails.
    Status crop_resize( int src_fd, int w_src, int h_src, LbGeom crop_roi, int dst_fd, int w_dst, int h_dst,
                        int aux_fd = -1, int w_aux = 0, int h_aux = 0 ) const noexcept;

private:
    // Pass 1 of the two-pass path: src (src_fmt_rk) into aux (NV12, w_aux x h_aux).
    Status downscale_to_aux( int src_fd, int w_src, int h_src, int src_w_stride, int src_h_stride,
                             int src_fmt_rk, int aux_fd, int w_aux, int h_aux ) const;

private:
    const Log& log_;
};
