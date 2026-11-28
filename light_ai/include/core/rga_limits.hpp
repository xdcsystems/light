#pragma once

#include <cstddef>
#include <cstdint>

// RK3588 RGA hardware limits as pure functions (SDK-free: host-tested in
// test_rga_limits). RgaResizer, SourceChannel, Pipeline and RknnEngine share
// them, so the aux-buffer decision and its size cannot drift apart.
//  - RGA3 (the core every job is pinned to) rejects any side < 68 px
//    (hardware probe: 64x64 -> st=0).
//  - One pass scales at most 8x per axis (down or up).
//  - RGB888 buffers need a 16-aligned width stride ("width stride should
//    be 16 aligned"); YUV420SP/422SP rects need even sizes.
namespace rga_limits
{
    constexpr int kMinPx = 68;
    constexpr int kMaxRatio = 8;
    constexpr int kStrideAlign = 16;

    constexpr int align16( int v ) noexcept
    {
        return ( v + 15 ) & ~15;
    }

    constexpr int even_down( int v ) noexcept
    {
        return v & ~1;
    }

    // Both sides of a rect are large enough for RGA3.
    constexpr bool rga3_size_ok( int w, int h ) noexcept
    {
        return w >= kMinPx && h >= kMinPx;
    }

    // One axis of one pass: src -> dst within 1/8x .. 8x.
    constexpr bool ratio_ok( int src, int dst ) noexcept
    {
        return src > 0 && dst > 0 && src <= kMaxRatio * dst && dst <= kMaxRatio * src;
    }

    // A two-pass job src -> aux -> dst: each pass is within the 8x limit on each axis.
    constexpr bool two_pass_ratio_ok( int src_w, int src_h, int aux_w, int aux_h, int dst_w, int dst_h ) noexcept
    {
        return ratio_ok( src_w, aux_w ) && ratio_ok( src_h, aux_h ) && ratio_ok( aux_w, dst_w ) && ratio_ok( aux_h, dst_h );
    }

    // The source downscale into the dst tensor exceeds 8x on an axis (with a
    // quarter turn the source axes swap): the two-pass path through aux is needed.
    constexpr bool needs_two_pass( int w_src, int h_src, int w_dst, int h_dst, bool swap_wh ) noexcept
    {
        const int ew = swap_wh ? h_src : w_src;
        const int eh = swap_wh ? w_src : h_src;
        return ew > kMaxRatio * w_dst || eh > kMaxRatio * h_dst;
    }

    // Re-ID crops (Pipeline::run_reid_stage) come from the detector tensor
    // (model_w x model_h) in one crop_resize pass, with no aux buffer: the
    // largest crop, the whole tensor, must fit the 8x downscale into the
    // Re-ID input (reid_w x reid_h). True: some crops would fail -- Re-ID
    // stays off (the Pipeline ctor). YOLO 640x640 into OSNet 128x256 fits.
    constexpr bool reid_crop_needs_aux( int model_w, int model_h, int reid_w, int reid_h ) noexcept
    {
        return needs_two_pass( model_w, model_h, reid_w, reid_h, false );
    }

    // The aux (pass-1) side for a source side: half, rounded up to even
    // (NV12 needs even sides; plain v / 2 truncates an odd half).
    constexpr int aux_side( int v ) noexcept
    {
        return ( ( v / 2 ) + 1 ) & ~1;
    }

    // Bytes the NV12 aux buffer needs for a w x h source: 16-aligned strides,
    // x2 headroom over the 3/2 NV12 size (rga_resizer.hpp contract).
    constexpr size_t aux_bytes( int w, int h ) noexcept
    {
        return static_cast<size_t>( align16( aux_side( w ) ) ) * static_cast<size_t>( align16( aux_side( h ) ) ) * 2;
    }

    // An RGB888 tensor row of stride_bytes, width w px, is usable by RGA:
    // whole pixels, at least w wide, 16-aligned in pixels.
    constexpr bool rgb888_stride_ok( uint32_t stride_bytes, int w ) noexcept
    {
        return w > 0 && stride_bytes % 3 == 0 && stride_bytes / 3 >= static_cast<uint32_t>( w ) &&
               ( stride_bytes / 3 ) % kStrideAlign == 0;
    }

    // RGB888 stride in pixels; 0 (unknown) = the tight width.
    constexpr int rgb888_stride_px( uint32_t stride_bytes, int w ) noexcept
    {
        return stride_bytes >= 3 ? static_cast<int>( stride_bytes / 3 ) : w;
    }
}  // namespace rga_limits
