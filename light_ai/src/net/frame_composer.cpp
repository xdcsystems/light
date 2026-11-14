#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <im2d.h>
#include <rga.h>

#include "core/idle_plate.hpp"
#include "core/log.hpp"
#include "dma_sync.hpp"
#include "net/encoder_frame_pool.hpp"
#include "net/frame_composer.hpp"

namespace
{
    // Explicit strides: the VU plane lands at w * vstride -- exactly where
    // the DRM descriptor (EncoderFramePool) reports it.
    rga_buffer_t nv12( MppBuffer mb, const StreamLayout& lay )
    {
        return wrapbuffer_fd( mpp_buffer_get_fd( mb ), lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    }

    im_rect rect( const LbGeom& g )
    {
        return im_rect { g.left, g.top, g.w, g.h };
    }

    bool same( const LbGeom& a, const LbGeom& b )
    {
        return a.left == b.left && a.top == b.top && a.w == b.w && a.h == b.h;
    }

    // A band that matches no real placement: forces the gray refill when
    // a live frame lands in a slot / cell the plate was copied into.
    LbGeom plate_band()
    {
        return LbGeom { 0, 1, 0, 1 };
    }

    void fill_nv12_gray( MppBuffer mb )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
            dma::fill_and_flush( mpp_buffer_get_fd( mb ), p, 128, mpp_buffer_get_size( mb ) );
    }

    // Fill one canvas cell (NV12 semi-planar): Y rows + interleaved UV
    // rows, bounded to the rect. Multi-view only -- a whole-canvas memset
    // would erase every neighbor's tile: live ones gray until their next
    // publish, an offline one gray until reconnect.
    void fill_nv12_gray_rect( MppBuffer mb, int stride, int vstride, const LbGeom& rc )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
        {
            uint8_t* py = static_cast<uint8_t*>( p );
            const size_t ysz = static_cast<size_t>( stride ) * static_cast<size_t>( vstride );
            dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
            for ( int r = 0; r < rc.h; ++r )
                std::memset( py + static_cast<size_t>( rc.top + r ) * stride + rc.left, 128, static_cast<size_t>( rc.w ) );
            uint8_t* puv = py + ysz;
            for ( int r = 0; r < rc.h / 2; ++r )
                std::memset( puv + static_cast<size_t>( rc.top / 2 + r ) * stride + ( rc.left / 2 ) * 2, 128,
                             static_cast<size_t>( rc.w / 2 ) * 2 );
            dma::cache_clean( p, mpp_buffer_get_size( mb ) );
        }
    }

    // NO SIGNAL plate (idle canvas): IdlePlate rasterizes the label into
    // the Y plane once at init (core/idle_plate.hpp); the DMA sync is here.
    void draw_no_signal( MppBuffer mb, const StreamLayout& lay )
    {
        if ( !mb )
            return;
        uint8_t* py = static_cast<uint8_t*>( mpp_buffer_get_ptr( mb ) );
        if ( !py )
            return;
        dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
        IdlePlate plate;
        plate.draw( Plane8 { py, lay.w, lay.vstride }, lay );
        dma::cache_clean( py, mpp_buffer_get_size( mb ) );
    }

    bool rga_ok( IM_STATUS st, const Log& log, const char* what )
    {
        if ( static_cast<int>( st ) > 0 )
            return true;
        log.error() << "enc: " << what << " failed: " << imStrError( st ) << "\n";
        return false;
    }
}  // namespace

FrameComposer::FrameComposer( const EncoderConfig& cfg, const Log& log )
  : log_( log )
  , lay_( cfg.layout )
  , model_w_( cfg.model_w )
  , model_h_( cfg.model_h )
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

    // Both canvases are gray-filled by the pool alloc. Alloc failures throw
    // (fail loud at startup); a pool that failed has already self-cleaned.
    const size_t bytes = static_cast<size_t>( lay_.w ) * lay_.vstride * 3 / 2;
    std::string why;
    if ( !canvas_.alloc( bytes, MODULE_TAG, __FUNCTION__, &why ) )
        throw std::runtime_error( "net encoder: canvas: " + why );
    // Idle canvas (NO SIGNAL): stamped with the plate in every tile, once.
    // compose_idle() RGA-copies from it -- the NPU tensor is never touched.
    if ( !idle_.alloc( bytes, MODULE_TAG, __FUNCTION__, &why ) )
        throw std::runtime_error( "net encoder: idle canvas: " + why );
    draw_no_signal( idle_.buf[0], lay_ );
}

// GPU draw -> RGA ordering is guaranteed by sync_cpu() in
// OverlaySink::draw_and_publish, which runs before publish() on the
// same pipeline thread.
bool FrameComposer::compose( int tensor_fd, const TilePlacement& tp, int slot, MppBuffer out )
{
    const bool multi = lay_.nsrc > 1;

    MppBuffer target = multi ? canvas_.buf[0] : out;
    LbGeom& prev = bands_[multi ? tp.cell : slot];
    if ( prev.w > 0 && !same( prev, tp.dst ) )
    {
        if ( multi )
            fill_nv12_gray_rect( target, lay_.w, lay_.vstride, cell_rect( lay_, tp.cell ) );
        else
            fill_nv12_gray( target );
    }
    prev = tp.dst;

    rga_buffer_t src = wrapbuffer_fd( tensor_fd, model_w_, model_h_, RK_FORMAT_RGB_888 );
    rga_buffer_t dst = nv12( target, lay_ );
    if ( !rga_ok( improcess( src, dst, {}, rect( tp.src ), rect( tp.dst ), {}, 0 ), log_, "RGA convert" ) )
        return false;
    if ( multi && out != nullptr )
        return rga_ok( imcopy( dst, nv12( out, lay_ ) ), log_, "RGA composite copy" );
    return true;
}

bool FrameComposer::compose_idle( const bool* absent, int nabs, int slot, MppBuffer out )
{
    rga_buffer_t idl = nv12( idle_.buf[0], lay_ );
    if ( lay_.nsrc <= 1 )
    {
        // Single view: the idle canvas IS the frame.
        if ( out == nullptr )
            return true;
        bands_[slot] = plate_band();
        return rga_ok( imcopy( idl, nv12( out, lay_ ) ), log_, "RGA idle copy" );
    }
    // Multi-view: the plate tile into every absent cell of the persistent
    // canvas that does not hold it already (only compose() and
    // reset_canvas() write a cell besides this copy); live cells keep
    // their last picture.
    rga_buffer_t cvs = nv12( canvas_.buf[0], lay_ );
    for ( int s = 0; s < lay_.nsrc; ++s )
    {
        if ( !tile_absent( absent, nabs, s ) || same( bands_[s], plate_band() ) )
            continue;
        const im_rect tile = rect( cell_rect( lay_, s ) );
        if ( !rga_ok( improcess( idl, cvs, {}, tile, tile, {}, 0 ), log_, "RGA idle tile copy" ) )
            return false;
        bands_[s] = plate_band();
    }
    if ( out == nullptr )
        return true;
    return rga_ok( imcopy( cvs, nv12( out, lay_ ) ), log_, "RGA idle composite copy" );
}

// One CPU fill of the canvas per viewer session (0.9 / 1.8 MB for 2x1 /
// 2x2), the same path as the single-view slot refill. The canvas is only
// written and read by synchronous RGA calls on this thread: nothing is in
// flight on it here.
void FrameComposer::reset_canvas()
{
    if ( lay_.nsrc <= 1 )
        return;
    fill_nv12_gray( canvas_.buf[0] );
    bands_.fill( LbGeom {} );
}
