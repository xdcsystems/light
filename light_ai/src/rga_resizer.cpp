#include <cstring>
#include <iostream>

#include <im2d.h>
#include <rga.h>

#include "rga_resizer.hpp"

RgaResizer::RgaResizer()
{
    // RK3588: RGA3 (own IOMMU) + RGA2 (32-bit phys addressing, cannot map
    // dma-bufs above 4G). 1080p decoder buffers land above 4G — dmesg:
    // "rga_mm: RGA_MMU unsupported memory larger than 4G!", scheduler fell
    // back to RGA2 (core[4]). All jobs are pinned to RGA3 core0 for the
    // process lifetime; RGA2 is never needed here.
    const IM_STATUS st = imconfig( IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0 );
    if ( (int)st <= 0 )
    {
        std::cerr << "RGA: imconfig RGA3_CORE0 pin failed: " << imStrError( st ) << '\n';
    }
}

bool RgaResizer::resize( int src_fd, int w_src, int h_src, int dst_fd, int w_dst, int h_dst, int aux_fd, int w_aux, int h_aux, LbGeom* geom, int src_fmt )
{
    const int src_fmt_rk = src_fmt == 0 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YCbCr_422_SP;
    // RGA3 scales at most 1/8 per axis. NOTE: integer division hides the
    // limit (1920/224 == 8, but the real ratio is 8.57) — compare by
    // multiplication.
    if ( w_src > 8 * w_dst || h_src > 8 * h_dst )
    {
        if ( aux_fd < 0 )
        {
            std::cerr << "RGA: two-pass needed but aux_fd < 0\n";
            return false;
        }
        rga_buffer_t src = wrapbuffer_fd( src_fd, w_src, h_src, src_fmt_rk );
        rga_buffer_t mid = wrapbuffer_fd( aux_fd, w_aux, h_aux, RK_FORMAT_YCbCr_420_SP );
        IM_STATUS st = imresize( src, mid );
        if ( (int)st <= 0 )
        {
            std::cerr << "RGA resize (pass 1) failed: " << imStrError( st ) << '\n';
            return false;
        }
        src_fd = aux_fd;
        w_src = w_aux;
        h_src = h_aux;
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
        std::cerr << "RGA: aspect " << w_src << 'x' << h_src << " does not fit into " << w_dst << 'x' << h_dst << '\n';
        return false;
    }

    rga_buffer_t src = wrapbuffer_fd( src_fd, w_src, h_src, src_fmt_rk );
    rga_buffer_t dst = wrapbuffer_fd( dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888 );
    im_rect srect = { 0, 0, w_src, h_src };
    im_rect drect = { g.left, g.top, g.w, g.h };
    // improcess instead of imresize: the drect band keeps the bars intact,
    // RGA never touches rows outside it.
    IM_STATUS st = improcess( src, dst, {}, srect, drect, {}, 0 );
    if ( (int)st <= 0 )
    {
        std::cerr << "RGA resize failed: " << imStrError( st ) << '\n';
        return false;
    }
    if ( geom )
        *geom = g;
    return true;
}

LbGeom band_geom( int w_src, int h_src, int w_dst, int h_dst )
{
    // Single owner of the letterbox formula: resize() and the Pipeline
    // ctor both call this, so the RGA crop, the encoder crop and the
    // postprocess scale always agree. Orientation-aware uniform fit
    // (integer truncation): the fit dimension is the tighter one —
    // landscape letterboxes vertically, portrait horizontally.
    LbGeom g;
    const bool fit_by_h = ( h_src * w_dst > w_src * h_dst );  // portrait-ish
    if ( fit_by_h )
    {
        g.w = w_src * h_dst / h_src;
        if ( g.w > w_dst )
            g.w = w_dst;
        g.h = h_dst;
        g.left = ( w_dst - g.w ) / 2;
        g.top = 0;
    }
    else
    {
        g.w = w_dst;
        g.h = h_src * w_dst / w_src;
        if ( g.h > h_dst )
            g.h = h_dst;
        g.left = 0;
        g.top = ( h_dst - g.h ) / 2;
    }
    return g;
}
