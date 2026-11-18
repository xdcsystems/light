#include <cstddef>  // NULL and memset: the im2d headers and macros use them without including
#include <cstring>

#include <im2d.h>
#include <rga.h>

#include "rga_resizer.h"
#include "core/log.hpp"

namespace
{
    // 6-arg wrapbuffer_fd preserves MPP horizontal/vertical stride alignment.
    rga_buffer_t wrap( int fd, int w, int h, int w_stride, int h_stride, int fmt )
    {
        return wrapbuffer_fd( fd, w, h, fmt, w_stride, h_stride );
    }

    rga_buffer_t wrap( int fd, int w, int h, int fmt )
    {
        return wrap( fd, w, h, w, h, fmt );
    }

    int resolve_stride( int stride, int fallback ) noexcept
    {
        return stride > 0 ? stride : fallback;
    }

    bool can_use_rga3( int cw, int ch, int w_dst, int h_dst, bool two_pass, int w_aux, int h_aux ) noexcept
    {
        const bool pass1_ok = cw >= 68 && ch >= 68 && w_dst >= 68 && h_dst >= 68;
        return pass1_ok && ( !two_pass || ( w_aux >= 68 && h_aux >= 68 ) );
    }

    // The only expansion of imStrError: an im2d status <= 0 is a failure,
    // logged as "<what>: <the SDK's text>" and returned as Failed with the
    // raw IM_STATUS in sys().
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

    Status rga_check( IM_STATUS st, const Log& log, const char* what )
    {
        if ( static_cast<int>( st ) > 0 )
            return {};
        log.error() << what << ": " << imStrError( st ) << '\n';
        return sdk_status( Errc::Failed, what, static_cast<int>( st ) );
    }
}  // namespace

RgaResizer::RgaResizer( const Log& log ) : log_( log )
{
    // RK3588: RGA3 (own IOMMU) + RGA2 (32-bit phys addressing, cannot map
    // dma-bufs above 4G). 1080p decoder buffers land above 4G — dmesg:
    // "rga_mm: RGA_MMU unsupported memory larger than 4G!", scheduler fell
    // back to RGA2 (core[4]). All jobs are pinned to RGA3 core0 for the
    // process lifetime; RGA2 is never needed here.
    // Best effort: an unpinned RGA still works, rga_check logs the cause.
    static_cast<void>( rga_check( imconfig( IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 ), log_, "RGA: imconfig RGA3_CORE0 pin failed" ) );
}

Status RgaResizer::resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux,
                           LbGeom* geom, int src_fmt, int rot_deg, int src_w_stride, int src_h_stride )
{
    int pass_fmt = ( src_fmt == 0 ) ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_422_SP;
    int ws = resolve_stride( src_w_stride, w_src );
    int hs = resolve_stride( src_h_stride, h_src );
    const bool swap_wh = ( rot_deg == 90 || rot_deg == 270 );
    const bool over_limit = w_src > 8 * w_dst || h_src > 8 * h_dst ||
                            ( swap_wh && ( h_src > 8 * w_dst || w_src > 8 * h_dst ) );
    if ( over_limit )
    {
        if ( Status st = downscale_to_aux( src_fd, w_src, h_src, ws, hs, pass_fmt, aux_fd, w_aux, h_aux ); !st.ok() )
            return st;
        src_fd = aux_fd;
        w_src = w_aux;
        h_src = h_aux;
        ws = w_aux;
        hs = h_aux;
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

    rga_buffer_t src = wrap( src_fd, w_src, h_src, ws, hs, pass_fmt );
    rga_buffer_t dst = wrap( dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888 );
    if ( Status st = rga_check( improcess( src, dst, {}, { 0, 0, w_src, h_src }, { g.left, g.top, g.w, g.h }, {},
                                           rga_rot_usage( rot_deg ) ),
                                log_, "RGA resize failed" );
         !st.ok() )
        return st;
    if ( geom )
        *geom = g;
    return {};
}

Status RgaResizer::crop_resize( int src_fd, int w_src, int h_src, LbGeom crop_roi, int dst_fd, int w_dst, int h_dst,
                                int aux_fd, int w_aux, int h_aux ) const noexcept
{
    if ( src_fd < 0 || dst_fd < 0 || w_src <= 0 || h_src <= 0 || w_dst <= 0 || h_dst <= 0 )
        return make_status( Errc::Invalid, "RGA crop: invalid buffer or size" );

    const int cx = crop_roi.left & ~1;
    const int cy = crop_roi.top & ~1;
    const int cw = crop_roi.w & ~1;
    const int ch = crop_roi.h & ~1;
    if ( cx < 0 || cy < 0 || cw <= 0 || ch <= 0 || cx + cw > w_src || cy + ch > h_src )
        return make_status( Errc::Invalid, "RGA crop: ROI out of bounds" );

    if ( cw * 8 < w_dst || ch * 8 < h_dst )
        return make_status( Errc::Invalid, "RGA crop: exceeds 8x upscale limit" );

    im_opt_t opt {};
    opt.core = can_use_rga3( cw, ch, w_dst, h_dst, cw > 8 * w_dst || ch > 8 * h_dst, w_aux, h_aux )
                   ? IM_SCHEDULER_RGA3_CORE0
                   : IM_SCHEDULER_RGA2_CORE0;

    im_rect sr { cx, cy, cw, ch };
    if ( cw > 8 * w_dst || ch > 8 * h_dst )
    {
        if ( aux_fd < 0 || w_aux <= 0 || h_aux <= 0 )
            return make_status( Errc::Failed, "RGA crop: >8x downscale without aux_fd" );
        if ( Status st = rga_check( improcess( wrap( src_fd, w_src, h_src, RK_FORMAT_RGB_888 ),
                                               wrap( aux_fd, w_aux, h_aux, RK_FORMAT_RGB_888 ),
                                               {}, sr, { 0, 0, w_aux, h_aux }, {}, -1, nullptr, &opt, IM_SYNC ),
                                    log_, "RGA crop (pass 1) failed" );
             !st.ok() )
            return st;
        src_fd = aux_fd;
        w_src = w_aux;
        h_src = h_aux;
        sr = { 0, 0, w_src, h_src };
    }

    return rga_check( improcess( wrap( src_fd, w_src, h_src, RK_FORMAT_RGB_888 ),
                                 wrap( dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888 ),
                                 {}, sr, { 0, 0, w_dst, h_dst }, {}, -1, nullptr, &opt, IM_SYNC ),
                      log_, "RGA crop failed" );
}

// Pass 1 of the two-pass path: the source into the NV12 aux buffer.
Status RgaResizer::downscale_to_aux( int src_fd, int w_src, int h_src, int src_w_stride, int src_h_stride,
                                     int src_fmt_rk, int aux_fd, int w_aux, int h_aux ) const
{
    if ( aux_fd < 0 )
    {
        log_.error() << "RGA: two-pass needed but aux_fd < 0\n";
        return make_status( Errc::Failed, "RGA: two-pass needed but aux_fd < 0" );
    }
    rga_buffer_t src = wrap( src_fd, w_src, h_src, src_w_stride, src_h_stride, src_fmt_rk );
    rga_buffer_t mid = wrap( aux_fd, w_aux, h_aux, RK_FORMAT_YCbCr_420_SP );
    return rga_check( imresize( src, mid ), log_, "RGA resize (pass 1) failed" );
}
