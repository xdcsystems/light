#include <cstddef>  // NULL and memset: the im2d headers and macros use them without including
#include <cstring>

#include <im2d.h>
#include <rga.h>

#include "media/rga_resizer.hpp"
#include "core/log.hpp"
#include "core/rga_limits.hpp"
#include "media/rga_status.hpp"

namespace
{
    // 6-arg wrapbuffer_fd preserves MPP horizontal/vertical stride alignment.
    rga_buffer_t wrap( int fd, int w, int h, int w_stride, int h_stride, int fmt )
    {
        return wrapbuffer_fd( fd, w, h, fmt, w_stride, h_stride );
    }

    using rga_limits::align16;

    // An unknown (0) stride: the 16-aligned size, how every frame buffer here is laid out.
    int resolve_stride( int stride, int size ) noexcept
    {
        return stride > 0 ? stride : align16( size );
    }

    bool can_use_rga3( int cw, int ch, int w_dst, int h_dst, bool two_pass, int w_aux, int h_aux ) noexcept
    {
        const bool pass1_ok = rga_limits::rga3_size_ok( cw, ch ) && rga_limits::rga3_size_ok( w_dst, h_dst );
        return pass1_ok && ( !two_pass || rga_limits::rga3_size_ok( w_aux, h_aux ) );
    }

    // The two-pass crop (cw x ch -> aux -> dst) needs an aux buffer, and the 8x limit holds for each pass on its own:
    // an aux size far from either end is named here, not by im2d. A one-pass crop needs nothing.
    Status check_crop_aux( bool two_pass, const RgaResizer::AuxImage& aux, int cw, int ch, int w_dst, int h_dst ) noexcept
    {
        if ( !two_pass )
            return {};
        if ( aux.fd < 0 || aux.w <= 0 || aux.h <= 0 )
            return make_status( Errc::Failed, "RGA crop: >8x downscale without aux_fd" );
        if ( !rga_limits::two_pass_ratio_ok( cw, ch, aux.w, aux.h, w_dst, h_dst ) )
            return make_status( Errc::Invalid, "RGA crop: aux size outside the 8x limit of a pass" );
        return {};
    }

    int rga_rot_usage( int rot_deg ) noexcept
    {
        switch ( rot_deg )
        {
            case 90:
                return IM_HAL_TRANSFORM_ROT_90;
            case 180:
                return IM_HAL_TRANSFORM_ROT_180;
            case 270:
                return IM_HAL_TRANSFORM_ROT_270;
            default:
                return 0;
        }
    }
}  // namespace

Status RgaResizer::resize( const ResizeSrc& src_img, const CropImage& dst_img, const AuxImage& aux, int rot_deg,
                           LbGeom* geom ) const
{
    const int w_dst = dst_img.w;
    const int h_dst = dst_img.h;
    // The pass-2 source: src_img, or aux after pass 1.
    int src_fd = src_img.fd;
    int w_src = src_img.w;
    int h_src = src_img.h;
    int pass_fmt = ( src_img.fmt == 0 ) ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_422_SP;
    int ws = resolve_stride( src_img.w_stride, w_src );
    int hs = resolve_stride( src_img.h_stride, h_src );
    const bool swap_wh = ( rot_deg == 90 || rot_deg == 270 );
    // Same predicate as SourceChannel::needs_aux (both read rga_limits), so a
    // two-pass frame always finds its aux buffer allocated.
    const bool over_limit = rga_limits::needs_two_pass( w_src, h_src, w_dst, h_dst, false ) ||
                            ( swap_wh && rga_limits::needs_two_pass( w_src, h_src, w_dst, h_dst, true ) );
    if ( over_limit )
    {
        // One even size for both passes (NV12): pass 2 reads with the rect and the strides pass 1 wrote with.
        const AuxImage mid { aux.fd, rga_limits::even_down( aux.w ), rga_limits::even_down( aux.h ) };
        if ( Status st = downscale_to_aux( src_img, pass_fmt, mid ); !st.ok() )
            return st;
        src_fd = mid.fd;
        w_src = mid.w;
        h_src = mid.h;
        ws = align16( mid.w );
        hs = align16( mid.h );
        pass_fmt = RK_FORMAT_YCbCr_420_SP;
    }

    const int eff_w = swap_wh ? h_src : w_src;
    const int eff_h = swap_wh ? w_src : h_src;
    const LbGeom g = band_geom( eff_w, eff_h, w_dst, h_dst );
    if ( g.h > h_dst || g.w > w_dst )
    {
        log_.error() << "RGA: aspect " << eff_w << 'x' << eff_h << " does not fit into " << w_dst << 'x' << h_dst << '\n';
        return make_status( Errc::Failed, "RGA: aspect does not fit" );
    }
    // The band is the real pass-2 dst rect: RGA3 rejects a side < 68 px (an
    // extreme aspect) and a single pass beyond 8x (a source > 16x the model
    // even after the halving aux pass). Checked here so the cause is named
    // instead of an opaque im2d error. Known edge: band_geom floors the band,
    // so a source within one pixel of exactly 8x (e.g. 5119 px into 640 ->
    // band 639) passes needs_two_pass yet fails here -- no real source is near it.
    if ( !rga_limits::rga3_size_ok( g.w, g.h ) || !rga_limits::ratio_ok( eff_w, g.w ) || !rga_limits::ratio_ok( eff_h, g.h ) )
    {
        log_.error() << "RGA: " << eff_w << 'x' << eff_h << " -> band " << g.w << 'x' << g.h
                     << " is outside the RGA3 limits (side >= 68 px, <= 8x per pass)\n";
        return make_status( Errc::Invalid, "RGA: band outside RGA3 limits" );
    }

    // YUV420SP/422SP source rects must have even sides; a decoder that hands
    // out an odd size loses its last row/column instead of failing the job.
    const rga_buffer_t src = wrap( src_fd, w_src, h_src, ws, hs, pass_fmt );
    // The tensor rows as RknnEngine bound them: every reader (NPU, drawer,
    // composer, dumper, preview) uses this stride, so the writer must too.
    const rga_buffer_t dst = wrap( dst_img.fd, w_dst, h_dst, dst_img.w_stride > 0 ? dst_img.w_stride : w_dst, h_dst, RK_FORMAT_RGB_888 );
    const im_rect src_rect { 0, 0, rga_limits::even_down( w_src ), rga_limits::even_down( h_src ) };
    im_opt_t opt {};
    opt.core = next_rga3_core();
    if ( Status st = rga_check( improcess( src, dst, {}, src_rect, { g.left, g.top, g.w, g.h }, {}, -1, nullptr, &opt,
                                           IM_SYNC | rga_rot_usage( rot_deg ) ),
                                log_, "RGA resize failed" );
         !st.ok() )
        return st;
    if ( geom )
        *geom = g;
    return {};
}

Status RgaResizer::crop_resize( const CropImage& src_img, LbGeom crop_roi, const CropImage& dst_img,
                                const AuxImage& aux ) const noexcept
{
    const int aux_fd = aux.fd;
    const int w_aux = aux.w;
    const int h_aux = aux.h;
    const int w_dst = dst_img.w;
    const int h_dst = dst_img.h;
    if ( src_img.fd < 0 || dst_img.fd < 0 || src_img.w <= 0 || src_img.h <= 0 || w_dst <= 0 || h_dst <= 0 )
        return make_status( Errc::Invalid, "RGA crop: invalid buffer or size" );
    // Row strides in pixels (0 = tight); RknnEngine::setup_io guarantees the
    // tensors are whole RGB888 pixels and 16-aligned.
    CropImage cur = src_img;  // the pass-2 source: the tensor, or aux after pass 1
    if ( cur.w_stride <= 0 )
        cur.w_stride = cur.w;
    int cur_h_stride = cur.h;

    const int cx = crop_roi.left & ~1;
    const int cy = crop_roi.top & ~1;
    const int cw = crop_roi.w & ~1;
    const int ch = crop_roi.h & ~1;
    if ( cx < 0 || cy < 0 || cw <= 0 || ch <= 0 || cx + cw > cur.w || cy + ch > cur.h )
        return make_status( Errc::Invalid, "RGA crop: ROI out of bounds" );

    if ( cw * rga_limits::kMaxRatio < w_dst || ch * rga_limits::kMaxRatio < h_dst )
        return make_status( Errc::Invalid, "RGA crop: exceeds 8x upscale limit" );

    const bool two_pass = cw > rga_limits::kMaxRatio * w_dst || ch > rga_limits::kMaxRatio * h_dst;
    // Before the core choice: without an aux the RGA2 test below would name the wrong cause.
    if ( Status st = check_crop_aux( two_pass, aux, cw, ch, w_dst, h_dst ); !st.ok() )
        return st;
    const bool rga3 = can_use_rga3( cw, ch, w_dst, h_dst, two_pass, w_aux, h_aux );
    // RGA2 maps only physical memory < 4 GiB (Rockchip RGA FAQ A1.9); aux has no dma32 guarantee.
    if ( !rga3 && ( !src_img.dma32 || !dst_img.dma32 || two_pass ) )
        return make_status( Errc::Failed, "RGA2 requires DMA32 buffer (< 4 GiB)" );
    im_opt_t opt {};
    opt.core = rga3 ? IM_SCHEDULER_RGA3_CORE0 : IM_SCHEDULER_RGA2_CORE0;

    im_rect sr { cx, cy, cw, ch };
    if ( two_pass )
    {
        // aux is RGB888 with 16-aligned strides (see crop_resize in rga_resizer.hpp).
        if ( Status st = rga_check( improcess( wrap( cur.fd, cur.w, cur.h, cur.w_stride, cur.h, RK_FORMAT_RGB_888 ),
                                               wrap( aux_fd, w_aux, h_aux, align16( w_aux ), align16( h_aux ), RK_FORMAT_RGB_888 ),
                                               {}, sr, { 0, 0, w_aux, h_aux }, {}, -1, nullptr, &opt, IM_SYNC ),
                                    log_, "RGA crop (pass 1) failed" );
             !st.ok() )
            return st;
        cur = CropImage { aux_fd, w_aux, h_aux, align16( w_aux ) };
        cur_h_stride = align16( h_aux );
        sr = { 0, 0, w_aux, h_aux };
    }

    const int wd = dst_img.w_stride > 0 ? dst_img.w_stride : w_dst;
    return rga_check( improcess( wrap( cur.fd, cur.w, cur.h, cur.w_stride, cur_h_stride, RK_FORMAT_RGB_888 ),
                                 wrap( dst_img.fd, w_dst, h_dst, wd, h_dst, RK_FORMAT_RGB_888 ),
                                 {}, sr, { 0, 0, w_dst, h_dst }, {}, -1, nullptr, &opt, IM_SYNC ),
                      log_, "RGA crop failed" );
}

// Pass 1 of the two-pass path: the source into the NV12 aux buffer (aux.w / aux.h even, see resize()).
Status RgaResizer::downscale_to_aux( const ResizeSrc& src_img, int src_fmt_rk, const AuxImage& aux ) const
{
    if ( aux.fd < 0 || aux.w <= 0 || aux.h <= 0 )
    {
        log_.error() << "RGA: two-pass needed but aux_fd < 0\n";
        return make_status( Errc::Failed, "RGA: two-pass needed but aux_fd < 0" );
    }
    const rga_buffer_t src = wrap( src_img.fd, src_img.w, src_img.h, resolve_stride( src_img.w_stride, src_img.w ),
                                   resolve_stride( src_img.h_stride, src_img.h ), src_fmt_rk );
    const rga_buffer_t mid = wrap( aux.fd, aux.w, aux.h, align16( aux.w ), align16( aux.h ), RK_FORMAT_YCbCr_420_SP );
    // An odd source side is cropped down to even here too (the first pass reads YUV).
    const im_rect src_rect { 0, 0, rga_limits::even_down( src_img.w ), rga_limits::even_down( src_img.h ) };
    im_opt_t opt {};
    opt.core = next_rga3_core();
    return rga_check( improcess( src, mid, {}, src_rect, { 0, 0, aux.w, aux.h }, {}, -1, nullptr, &opt, IM_SYNC ),
                      log_, "RGA resize (pass 1) failed" );
}

int RgaResizer::next_rga3_core() const noexcept
{
    const uint32_t idx = rga3_rr_.fetch_add( 1U, std::memory_order_relaxed );
    return ( idx & 1U ) == 0U ? IM_SCHEDULER_RGA3_CORE0 : IM_SCHEDULER_RGA3_CORE1;
}
