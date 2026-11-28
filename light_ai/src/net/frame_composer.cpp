#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <im2d.h>
#include <rga.h>

#include "core/idle_plate.hpp"
#include "core/log.hpp"
#include "core/rga_limits.hpp"
#include "core/throw_on.hpp"
#include "media/dma_sync.hpp"
#include "media/rga_status.hpp"
#include "net/encoder_frame_pool.hpp"
#include "net/frame_composer.hpp"

namespace
{
    im_opt_t rga3_opt() noexcept
    {
        im_opt_t opt {};
        opt.core = IM_SCHEDULER_RGA3_CORE0;
        return opt;
    }

    // Explicit strides: the VU plane lands at w * vstride -- exactly where
    // the DRM descriptor (EncoderFramePool) reports it.
    rga_buffer_t nv12( MppBuffer mb, const StreamLayout& lay ) noexcept
    {
        return wrapbuffer_fd( mpp_buffer_get_fd( mb ), lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    }

    // Output canvas (RGB888): lay.w is 16-aligned by make_stream_layout(),
    // matching both RGA3 RGB888 stride rules and Mali G610 DRM_FORMAT_RGB888 pitch.
    rga_buffer_t rgb888( MppBuffer mb, const StreamLayout& lay ) noexcept
    {
        return wrapbuffer_fd( mpp_buffer_get_fd( mb ), lay.w, lay.h, RK_FORMAT_RGB_888, lay.w, lay.h );
    }

    im_rect rect( const LbGeom& g ) noexcept
    {
        return im_rect { g.left, g.top, g.w, g.h };
    }

    bool same( const LbGeom& a, const LbGeom& b ) noexcept
    {
        return a.left == b.left && a.top == b.top && a.w == b.w && a.h == b.h;
    }

    // A band that matches no real placement: forces the gray refill when
    // a live frame lands in a cell the plate was copied into.
    LbGeom plate_band() noexcept
    {
        return LbGeom { 0, 1, 0, 1 };
    }

    void fill_rgb888_gray( MppBuffer mb )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
            dma::fill_and_flush( mpp_buffer_get_fd( mb ), p, 128, mpp_buffer_get_size( mb ) );
    }

    // Fill one canvas cell (RGB888) neutral gray (128, 128, 128) without
    // touching neighboring tiles. Targeted cache drop/clean on the cell rows.
    void fill_rgb888_gray_rect( MppBuffer mb, int stride_px, const LbGeom& rc )
    {
        if ( rc.w <= 0 || rc.h <= 0 )
            return;
        void* p = mpp_buffer_get_ptr( mb );
        if ( p == nullptr )
            return;
        const size_t row_stride = static_cast<size_t>( stride_px ) * 3;
        const size_t dirty_off = static_cast<size_t>( rc.top ) * row_stride;
        const size_t dirty_len = static_cast<size_t>( rc.h ) * row_stride;
        uint8_t* base = static_cast<uint8_t*>( p );
        const dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
        dma::cache_drop( base + dirty_off, dirty_len );
        for ( int r = 0; r < rc.h; ++r )
        {
            uint8_t* dst = base + static_cast<size_t>( rc.top + r ) * row_stride + static_cast<size_t>( rc.left ) * 3;
            std::memset( dst, 128, static_cast<size_t>( rc.w ) * 3 );
        }
        dma::cache_clean( base + dirty_off, dirty_len );
    }

    // NO SIGNAL plate (idle NV12 canvas): IdlePlate blends the label into the gray
    // Y plane (read-modify-write) once at init (core/idle_plate.hpp); the DMA
    // sync is here (DmaSync RW: the pixels are read too).
    void draw_no_signal( MppBuffer mb, const StreamLayout& lay )
    {
        if ( !mb )
            return;
        uint8_t* py = static_cast<uint8_t*>( mpp_buffer_get_ptr( mb ) );
        if ( !py )
            return;
        const dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
        const IdlePlate plate {};
        plate.draw( Plane8 { py, lay.w, lay.vstride }, lay );
        dma::cache_clean( py, mpp_buffer_get_size( mb ) );
    }
}  // namespace

FrameComposer::FrameComposer( const EncoderConfig& cfg, const Log& log )
  : log_( log )
  , lay_( cfg.layout )
  , model_w_( cfg.model_w )
  , model_h_( cfg.model_h )
  , model_stride_px_( cfg.model_stride_px > 0 ? cfg.model_stride_px : cfg.model_w )
{
    static_assert( static_cast<int>( EncoderFramePool::kSlots ) <= static_cast<int>( kBands ), "one band per ring slot" );
    static_assert( static_cast<int>( StreamLayout::kMaxTiles ) <= static_cast<int>( kBands ), "one band per grid cell" );

    // The stream geometry (single view: the band at its native size;
    // multi-view: one cell per source), logged before the canvases and the
    // codec come up.
    if ( lay_.nsrc > 1 )
        log_.info() << "enc: multi-view " << lay_.nsrc << " sources, tile " << lay_.tile_w << 'x' << lay_.tile_h
                    << " -> " << lay_.w << 'x' << lay_.h << '\n';
    else
        log_.info() << "enc: tensor band x=[" << lay_.band.left << ',' << lay_.band.left + lay_.band.w << ") y=["
                    << lay_.band.top << ',' << lay_.band.top + lay_.band.h << ") of " << model_w_ << 'x' << model_h_
                    << " -> " << lay_.w << 'x' << lay_.h << '\n';

    // Persistent RGB888 output canvas (gray-filled by pool alloc); overlay draws
    // 1:1 on this buffer before flush_canvas() converts RGB888 -> NV12 for MPP.
    const size_t canvas_bytes = static_cast<size_t>( lay_.w ) * static_cast<size_t>( lay_.h ) * 3;
    throw_on( canvas_.alloc( canvas_bytes, MODULE_TAG, __FUNCTION__ ), "net encoder: canvas" );
    // Idle canvas (NO SIGNAL, NV12): stamped with the plate in every tile, once.
    // compose_idle() RGA-copies from it -- the NPU tensor is never touched.
    const size_t idle_bytes = static_cast<size_t>( lay_.w ) * static_cast<size_t>( lay_.vstride ) * 3 / 2;
    throw_on( idle_.alloc( idle_bytes, MODULE_TAG, __FUNCTION__ ), "net encoder: idle canvas" );
    draw_no_signal( idle_.buf[0], lay_ );
}

CanvasBuffer FrameComposer::canvas_buffer() const noexcept
{
    MppBuffer mb = canvas_.buf[0];
    if ( !mb )
        return {};
    return CanvasBuffer {
        mpp_buffer_get_fd( mb ),
        mpp_buffer_get_ptr( mb ),
        mpp_buffer_get_size( mb ),
        lay_.w,
        lay_.h,
        static_cast<uint32_t>( lay_.w ) * 3
    };
}

Status FrameComposer::compose_tile( int tensor_fd, const TilePlacement& tp )
{
    const bool multi = lay_.nsrc > 1;
    const int cell = multi ? tp.cell : 0;
    MppBuffer target = canvas_.buf[0];
    LbGeom& prev = bands_[cell];
    if ( prev.w > 0 && !same( prev, tp.dst ) )
    {
        if ( multi )
            fill_rgb888_gray_rect( target, lay_.w, cell_rect( lay_, cell ) );
        else
            fill_rgb888_gray( target );
    }
    prev = tp.dst;

    const rga_buffer_t src = wrapbuffer_fd( tensor_fd, model_w_, model_h_, RK_FORMAT_RGB_888, model_stride_px_, model_h_ );
    const rga_buffer_t dst = rgb888( target, lay_ );
    im_opt_t opt = rga3_opt();
    if ( !rga_limits::rga3_size_ok( tp.src.w, tp.src.h ) || !rga_limits::rga3_size_ok( tp.dst.w, tp.dst.h ) )
        opt.core = IM_SCHEDULER_RGA2_CORE0;
    return rga_check( improcess( src, dst, {}, rect( tp.src ), rect( tp.dst ), {}, -1, nullptr, &opt, IM_SYNC ),
                      log_, "enc: RGA tile compose failed" );
}

Status FrameComposer::flush_canvas( MppBuffer out )
{
    if ( out == nullptr )
        return {};
    const rga_buffer_t cvs = rgb888( canvas_.buf[0], lay_ );
    const im_rect full { 0, 0, lay_.w, lay_.h };
    im_opt_t opt = rga3_opt();
    return rga_check( improcess( cvs, nv12( out, lay_ ), {}, full, full, {}, -1, nullptr, &opt, IM_SYNC ),
                      log_, "enc: RGA canvas->NV12 failed" );
}

Status FrameComposer::compose_idle( const bool* absent, int nabs, int /*slot*/, MppBuffer out )
{
    const rga_buffer_t idl = nv12( idle_.buf[0], lay_ );
    im_opt_t opt = rga3_opt();
    if ( lay_.nsrc <= 1 )
    {
        // Single view: the NV12 idle canvas IS the frame (direct copy into out).
        if ( out == nullptr )
            return {};
        bands_[0] = plate_band();
        const im_rect full { 0, 0, lay_.w, lay_.h };
        return rga_check( improcess( idl, nv12( out, lay_ ), {}, full, full, {}, -1, nullptr, &opt, IM_SYNC ),
                          log_, "enc: RGA idle copy failed" );
    }
    // Multi-view: convert the NV12 plate tile into every absent cell of the
    // persistent RGB888 canvas that does not hold it already; live cells keep
    // their last picture + 1:1 overlay.
    const rga_buffer_t cvs = rgb888( canvas_.buf[0], lay_ );
    for ( int s = 0; s < lay_.nsrc; ++s )
    {
        if ( !tile_absent( absent, nabs, s ) || same( bands_[s], plate_band() ) )
            continue;
        const im_rect tile = rect( cell_rect( lay_, s ) );
        if ( const Status tst = rga_check( improcess( idl, cvs, {}, tile, tile, {}, -1, nullptr, &opt, IM_SYNC ),
                                           log_, "enc: RGA idle tile copy failed" );
             !tst.ok() )
            return tst;
        bands_[s] = plate_band();
    }
    if ( out == nullptr )
        return {};
    return flush_canvas( out );
}

// One CPU fill of the RGB888 canvas per viewer session (1.8 / 3.7 MB for 2x1 /
// 2x2). The canvas is only written and read by synchronous RGA / GPU-synced
// calls on this worker thread: nothing is in flight on it here.
void FrameComposer::reset_canvas()
{
    if ( lay_.nsrc <= 1 )
        return;
    fill_rgb888_gray( canvas_.buf[0] );
    bands_.fill( LbGeom {} );
}
