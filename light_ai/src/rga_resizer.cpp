#include <cstddef>  // NULL and memset: the im2d headers and macros use them without including
#include <cstring>

#include <im2d.h>
#include <rga.h>

#include "rga_resizer.h"
#include "core/log.hpp"

namespace
{
    // The only expansion of the im2d wrapbuffer_fd macro (a statement
    // expression on its vararg count): the no-stride form, i.e. the SDK
    // picks wstride = width, hstride = height.
    rga_buffer_t wrap( int fd, int w, int h, int fmt )
    {
        return wrapbuffer_fd( fd, w, h, fmt );
    }

    // The only expansion of imStrError: an im2d status <= 0 is a failure,
    // logged as "<what>: <the SDK's text>" and returned as Failed with the
    // raw IM_STATUS in sys().
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

Status RgaResizer::resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux, LbGeom* geom, int src_fmt )
{
    const int src_fmt_rk = src_fmt == 0 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_422_SP;
    // Pass 2 must describe the buffer it actually reads. The intermediate
    // is always NV12, even when the camera frame was 4:2:2. Keeping
    // src_fmt_rk here makes RGA read that NV12 as NV16 and walk off the
    // aux allocation (1080p two-pass path).
    int pass_fmt = src_fmt_rk;
    // RGA3 scales at most 1/8 per axis. NOTE: integer division hides the
    // limit (1920/224 == 8, but the real ratio is 8.57) — compare by
    // multiplication.
    if ( w_src > 8 * w_dst || h_src > 8 * h_dst )
    {
        if ( Status st = downscale_to_aux( src_fd, w_src, h_src, src_fmt_rk, aux_fd, w_aux, h_aux ); !st.ok() )
            return st;
        src_fd = aux_fd;
        w_src = w_aux;
        h_src = h_aux;
        pass_fmt = RK_FORMAT_YCbCr_420_SP;
    }

    // Letterbox: orientation-aware uniform fit. Landscape sources
    // letterbox vertically (visible band is w_dst x lb_h at y = lb_top),
    // portrait ones horizontally (lb_w x h_dst at x = lb_left).
    // Examples (224-model): 1280x720 -> lb_h 126, top 49; 1920x1080
    // (after pass 1, aux 960x540) -> the same 126/49; 640x480 -> 168/28;
    // 720x1280 -> lb_w 126, left 49.
    const LbGeom g = band_geom( w_src, h_src, w_dst, h_dst );
    if ( g.h > h_dst || g.w > w_dst )
    {
        log_.error() << "RGA: aspect " << w_src << 'x' << h_src << " does not fit into " << w_dst << 'x' << h_dst << '\n';
        return make_status( Errc::Failed, "RGA: aspect does not fit" );
    }

    rga_buffer_t src = wrap( src_fd, w_src, h_src, pass_fmt );
    rga_buffer_t dst = wrap( dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888 );
    im_rect srect = { 0, 0, w_src, h_src };
    im_rect drect = { g.left, g.top, g.w, g.h };
    // improcess instead of imresize: the drect band keeps the bars intact,
    // RGA never touches rows outside it.
    if ( Status st = rga_check( improcess( src, dst, {}, srect, drect, {}, 0 ), log_, "RGA resize failed" ); !st.ok() )
        return st;
    if ( geom )
        *geom = g;
    return {};
}

// Pass 1 of the two-pass path: the source into the NV12 aux buffer.
Status RgaResizer::downscale_to_aux( int src_fd, int w_src, int h_src, int src_fmt_rk, int aux_fd, int w_aux, int h_aux ) const
{
    if ( aux_fd < 0 )
    {
        log_.error() << "RGA: two-pass needed but aux_fd < 0\n";
        return make_status( Errc::Failed, "RGA: two-pass needed but aux_fd < 0" );
    }
    rga_buffer_t src = wrap( src_fd, w_src, h_src, src_fmt_rk );
    rga_buffer_t mid = wrap( aux_fd, w_aux, h_aux, RK_FORMAT_YCbCr_420_SP );
    return rga_check( imresize( src, mid ), log_, "RGA resize (pass 1) failed" );
}
