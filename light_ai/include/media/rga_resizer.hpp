#pragma once

#include <atomic>
#include <cstdint>

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
    // One RGB888 image: dma-buf fd, size, row stride in pixels (0 = tight).
    // The crop_resize source and destination, the resize destination.
    struct CropImage
    {
        int fd = -1;
        int w = 0;
        int h = 0;
        int w_stride = 0;
        bool dma32 = false;  // physical memory < 4 GiB (TensorRef::dma32): required by the RGA2 route
    };

    // The resize source: a YUV semi-planar decoder frame. w_stride / h_stride:
    // 0 = the 16-aligned w / h (how MppDecoder allocates its frames).
    // fmt: 0 = YUV420SP, nonzero = YUV422SP -- the actual decoder output
    // layout (MppDecoder::out_fmt).
    struct ResizeSrc
    {
        int fd = -1;
        int w = 0;
        int h = 0;
        int w_stride = 0;
        int h_stride = 0;
        int fmt = 0;
    };

    // The intermediate buffer of a two-pass job: dma-buf fd and size, with
    // 16-aligned strides; fd < 0 = none.
    struct AuxImage
    {
        int fd = -1;
        int w = 0;
        int h = 0;
    };

    // Failures go to log. No process or thread default is set: every job names its core in im_opt_t. RGA3 (own
    // IOMMU) takes the jobs, because RGA2 cannot map memory above 4 GiB, where 1080p decoder buffers land; the one
    // RGA2 route is the small-crop case of crop_resize().
    explicit RgaResizer( const Log& log ) : log_( log ) {}
    // src: YUVSP (see ResizeSrc), dst: RGB888, its w_stride the tensor row
    // in pixels (TensorRef::stride_bytes / 3; 0 = dst.w).
    // aux: the intermediate for the two-pass path when the scale ratio
    // exceeds the RGA3 per-axis limit (1/8); {} if unused. It is NV12 with
    // 16-aligned strides: the caller allocates at least
    // align16(aux.w) * align16(aux.h) * 3 / 2 bytes (SourceChannel: x2).
    // The MJPEG decoder follows the stream subsampling and ignores
    // the fmt pre-set on its frame; the RGA read side must match.
    // rot_deg: 0 / 90 / 180 / 270, the rotation applied on the way.
    // Ok: *geom (if not null) receives the band geometry. A failure Status
    // carries the cause (aspect mismatch, a band outside the RGA3 limits of
    // core/rga_limits.hpp, no aux for the two-pass path, an im2d error
    // with the raw IM_STATUS in sys()); geom stays untouched then (h = 0
    // marks "no valid band").
    // An odd source side is cropped down to even (YUV rects need even sides).
    Status resize( const ResizeSrc& src, const CropImage& dst, const AuxImage& aux, int rot_deg, LbGeom* geom ) const;
    // Crops RGB888 sub-rect from model tensor -> dst RGB888 Re-ID tensor: routes >= 68x68
    // crops to RGA3 (IOMMU) and < 68 px crops to RGA2 (< 4G DMA32). Enforces 1/8x .. 8x scale bounds.
    // The RGA2 route needs dma32 on both src and dst and cannot take the two-pass path
    // (aux carries no dma32 guarantee); otherwise it fails without submitting a job.
    // src / dst carry their row strides (TensorRef::stride_bytes / 3; 0 = tight).
    // A > 8x downscale needs aux: an RGB888
    // aux.w x aux.h intermediate with 16-aligned strides (RGA rejects an unaligned
    // RGB888 width stride), so the caller allocates at least
    // align16(aux.w) * align16(aux.h) * 3 bytes; without aux that case fails.
    // Each of the two passes must stay within 8x per axis, else Invalid, no job.
    Status crop_resize( const CropImage& src, LbGeom crop_roi, const CropImage& dst, const AuxImage& aux ) const noexcept;

private:
    // Pass 1 of the two-pass path: src (its strides resolved, read as
    // src_fmt_rk) into aux (NV12).
    Status downscale_to_aux( const ResizeSrc& src, int src_fmt_rk, const AuxImage& aux ) const;
    int next_rga3_core() const noexcept;

private:
    const Log& log_;
    mutable std::atomic<uint32_t> rga3_rr_ { 0 };
};
