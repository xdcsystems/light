#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#include "app/preprocessor.hpp"
#include "core/rga_limits.hpp"
#include "media/dma_sync.hpp"

Preprocessor::Preprocessor( const Log& log ) : log_( log ), rga_( log ), cal_dumper_( log ) {}

void Preprocessor::init( const CalConfig& cal, int model_w, int model_h, uint32_t input_stride_bytes,
                         const TensorSet& tensors )
{
    model_w_ = model_w;
    model_h_ = model_h;

    // Gray letterbox bars on every pooled tensor: RGA writes only the
    // visible band (drect), so the 128-fill done here survives every frame.
    for ( int k = 0; k < tensors.n; ++k )
    {
        const TensorRef tn = tensors.d[k];
        dma::fill_and_flush( tn.fd, tn.data, 128, tn.size );
    }

    cal_dumper_.init( cal, model_w_, model_h_, input_stride_bytes );

    if ( cal.frames > 0 && cal.dir.empty() )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
    }
}

void Preprocessor::reset_orient( int src, const SourceChannel& ch ) noexcept
{
    const SourceGeom& ss = ch.geom();
    OrientState& os = orient_[static_cast<size_t>( src )];
    os.orient.reset( ch.rot_auto(), ch.rot_deg(), ss.raw_h > ss.raw_w );
    os.cur_rot = os.orient.active_rot();
}

void Preprocessor::prepare_tensor_band( int slot, const TensorRef& tn, const LbGeom& eff_lb ) noexcept
{
    LbGeom& band = tensor_band_[static_cast<size_t>( slot )];
    if ( tn.data != nullptr && band.w > 0 &&
         ( band.left != eff_lb.left || band.top != eff_lb.top || band.w != eff_lb.w || band.h != eff_lb.h ) )
    {
        dma::fill_and_flush( tn.fd, tn.data, 128, tn.size );
        band = {};
    }
}

Status Preprocessor::resize_into_slot( int slot, const SourceChannel& ch, int fd_src, const TensorRef& tn, int rot,
                                       SlotResize& out ) noexcept
{
    const SourceGeom& ss = ch.geom();  // Live: the frame thread owns it
    const int raw_w = ss.raw_w > 0 ? ss.raw_w : ss.w;
    const int raw_h = ss.raw_h > 0 ? ss.raw_h : ss.h;
    auto_orient::oriented_dims( raw_w, raw_h, rot, out.w, out.h );
    out.band = band_geom( out.w, out.h, model_w_, model_h_ );
    prepare_tensor_band( slot, tn, out.band );

    const RgaResizer::ResizeSrc src_img { fd_src, raw_w, raw_h, ch.hor_stride(), ch.ver_stride(),
                                          ch.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1 };
    const RgaResizer::CropImage dst_img { tn.fd, model_w_, model_h_,
                                          rga_limits::rgb888_stride_px( tn.stride_bytes, model_w_ ) };
    const RgaResizer::AuxImage aux { ss.aux_fd, rga_limits::aux_side( raw_w ), rga_limits::aux_side( raw_h ) };
    const Status st = rga_.resize( src_img, dst_img, aux, rot, &out.rga );
    if ( st.ok() )
        tensor_band_[static_cast<size_t>( slot )] = out.band;
    return st;
}

void Preprocessor::after_resize_ok( int src, int frame_idx, const TensorRef& tn, const SlotResize& rs ) noexcept
{
    cal_dumper_.maybe( frame_idx, tn.fd, static_cast<const uint8_t*>( tn.data ), tn.size );
    const LbGeom& lg = rs.rga;
    const LbGeom& eb = rs.band;
    if ( !lb_warned_ && ( lg.left != eb.left || lg.top != eb.top || lg.w != eb.w || lg.h != eb.h ) )
    {
        lb_warned_ = true;
        log_.warn() << "pipeline: RGA letterbox drift (src " << src << "): x=" << lg.left << " top=" << lg.top
                    << " w=" << lg.w << " h=" << lg.h
                    << " vs x=" << eb.left << " top=" << eb.top << " w=" << eb.w << " h=" << eb.h << '\n';
    }
}
