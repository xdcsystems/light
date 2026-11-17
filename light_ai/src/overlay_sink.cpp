#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

#include "overlay_sink.hpp"
#include "box_mapper.h"
#include "config/config.hpp"  // StreamConfig
#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "core/status.hpp"
#include "dma_sync.hpp"
#include "timings.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    // CPU baseline: 1-px outlines straight into the NPU tensor (RGB888).
    // The caller pre-maps boxes to tensor coordinates — the same geometry as the GPU path;
    // colors come from the box itself. Used when the GPU drawer
    // is unavailable, or forced by --cpu-draw (A/B baseline).
    // Geometry parity with the GPU path: fully off-screen and
    // degenerate (< 1 px) boxes are skipped, like the GPU viewport /
    // discard, and the pixel span uses floor/ceil like the rasterizer.
    // Porter-Duff alpha: the per-box alpha blends over the
    // existing tensor pixels; a == 255 stays an opaque write.
    void draw_boxes_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox* boxes, int n )
    {
        for ( int i = 0; i < n; ++i )
        {
            const GpuBox& b = boxes[i];
            if ( b.x1 <= 0.0f || b.x0 >= static_cast<float>( w ) || b.y1 <= 0.0f
                 || b.y0 >= static_cast<float>( h ) )
                continue;
            if ( b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f )
                continue;
            const int x0 = std::clamp( static_cast<int>( std::floor( b.x0 ) ), 0, w - 1 );
            const int y0 = std::clamp( static_cast<int>( std::floor( b.y0 ) ), 0, h - 1 );
            const int x1 = std::max( x0, std::clamp( static_cast<int>( std::ceil( b.x1 ) ) - 1, 0, w - 1 ) );
            const int y1 = std::max( y0, std::clamp( static_cast<int>( std::ceil( b.y1 ) ) - 1, 0, h - 1 ) );
            auto put = [&]( int x, int y ) {
                uint8_t* px = rgb + static_cast<size_t>( y ) * stride + static_cast<std::ptrdiff_t>( x ) * 3;
                const unsigned a = b.a;
                if ( a >= 255 )
                {
                    px[0] = b.r;
                    px[1] = b.g;
                    px[2] = b.b;
                    return;
                }
                px[0] = static_cast<uint8_t>( ( b.r * a + px[0] * ( 255 - a ) + 127 ) / 255 );
                px[1] = static_cast<uint8_t>( ( b.g * a + px[1] * ( 255 - a ) + 127 ) / 255 );
                px[2] = static_cast<uint8_t>( ( b.b * a + px[2] * ( 255 - a ) + 127 ) / 255 );
            };
            // Each boundary pixel is painted exactly once: Porter-Duff
            // over is not idempotent for a < 255, a double write makes
            // the corner pixels brighter than the edges (trail dots).
            for ( int x = x0; x <= x1; ++x )
            {
                put( x, y0 );
                if ( y1 > y0 )
                    put( x, y1 );
            }
            for ( int y = y0 + 1; y < y1; ++y )
            {
                put( x0, y );
                if ( x1 > x0 )
                    put( x1, y );
            }
        }
    }

    // StreamConfig -> EncoderConfig, field by field by name (core/ may not
    // include config/config.hpp). nsrc beyond the 2x2 grid is clamped by
    // make_stream_layout(); the warning is logged here.
    EncoderConfig encoder_config( const StreamConfig& stream, int model_w, int model_h, const LbGeom& band, int nsrc,
                                  const Log& log )
    {
        if ( nsrc > StreamLayout::kMaxTiles )
            log.warn() << "enc: clamping source count " << nsrc << " -> " << static_cast<int>( StreamLayout::kMaxTiles )
                       << " (2x2 grid max)\n";
        EncoderConfig c;
        c.layout = make_stream_layout( model_w, model_h, band, nsrc );
        c.model_w = model_w;
        c.model_h = model_h;
        c.fps = stream.fps;
        c.port = stream.port;
        c.host = stream.host;
        c.bitrate_kbps = stream.bitrate_kbps;  // the <= 0 fallback is EncoderConfig::kbps()
        return c;
    }

}  // namespace

void OverlaySink::init( const StreamConfig& stream,
                        const TensorSet& tensors,
                        int model_w,
                        int model_h,
                        const LbGeom& primary_band,
                        int nsrc )
{
    model_w_ = model_w;
    model_h_ = model_h;
    const uint32_t fallback_stride = static_cast<uint32_t>( model_w_ ) * 3;
    stride_bytes_ = ( tensors.n > 0 && tensors.d[0].stride_bytes > 0 ) ? tensors.d[0].stride_bytes : fallback_stride;
    if ( !stream.enabled )
        return;

    // Compare allocated mem against stride_bytes * model_h_ (avoiding tn.size / model_h_
    // which overestimates when native model input is FP16).
    const uint64_t expected_bytes = static_cast<uint64_t>( stride_bytes_ ) * static_cast<uint64_t>( model_h_ );
    log_.info() << "net: tensor stride check: input mem=" << tensors.d[0].size
                << " stride=" << stride_bytes_ << " expected=" << expected_bytes << "\n";
    if ( stride_bytes_ != fallback_stride )
        log_.warn() << "net: non-packed input tensor stride (" << stride_bytes_ << " != " << fallback_stride
                    << "); RGA producers assume packed RGB888\n";

    if ( stream.h264 )
    {
        const EncoderConfig ec = encoder_config( stream, model_w_, model_h_, primary_band, nsrc, log_ );
        enc_.emplace( ec, log_ );
        // kbps(): the rate the encoder runs at (the <= 0 fallback applied).
        if ( stream.host.empty() )
            log_.info() << "net: h264 PULL server on port " << stream.port << ", " << ec.kbps()
                        << " kbps (viewer: ffplay tcp://<board-ip>:" << stream.port << ")\n";
        else
            log_.info() << "net: h264 preview -> " << stream.host << ':' << stream.port << ", " << ec.kbps() << " kbps\n";
    }
    else
    {
        preview_.emplace( model_w_, model_h_, static_cast<int>( stride_bytes_ ), stream.fps, stream.port, log_ );
        log_.info() << "net: preview server on port " << stream.port << "\n";
    }

    init_gpu_drawer( stream.cpu_draw, tensors );
}

// NO SIGNAL idle publish: h264 path only -- the encoder RGA-copies the
// pre-rendered gray plate into the absent tiles. The raw NUT preview
// keeps its live-only diagnostic role (no idle frames there).
void OverlaySink::publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept
{
    if ( enc_ )
        enc_->publish_idle( ts_us, absent, nabs );
}

void OverlaySink::init_gpu_drawer( bool cpu_draw, const TensorSet& tensors )
{
    if ( ( !preview_ && !enc_ ) || cpu_draw )
        return;

    drawer_.emplace( log_ );

    Status drew_st = drawer_->init();
    for ( int k = 0; drew_st.ok() && k < tensors.n; ++k )
    {
        const uint32_t slot_stride = tensors.d[k].stride_bytes > 0 ? tensors.d[k].stride_bytes : stride_bytes_;
        drew_st = drawer_->attach_slot( k, tensors.d[k].fd, model_w_, model_h_, slot_stride );
    }

    if ( !drew_st.ok() )
    {
        log_.warn() << "pipeline: gpu drawer unavailable, CPU fallback\n";
        drawer_.reset();
    }
    else
    {
        log_.info() << "net: gpu box drawing active (Mali G610, EGL/GBM)\n";
    }
}

double OverlaySink::draw_and_publish( const FrameView& frame,
                                      const DetList& dets,
                                      const TrajList& trajs,
                                      const Tracker& tracker ) noexcept
{
    double enc_ms = 0.0;
    const bool enc_live = enc_ && enc_->viewer_active();
    const bool prev_live = preview_ && preview_->viewer_connected();
    if ( !enc_live && !prev_live )
        return enc_ms;

    const TensorRef& tn = frame.tensor;
    const float sx = frame.lb.w / static_cast<float>( frame.src_w );
    const float sy = frame.lb.h / static_cast<float>( frame.src_h );
    boxes_.clear();
    mapper_.fill( dets, trajs, tracker, boxes_, frame.lb, sx, sy, frame.ts_us );
    const bool cpu_drew = ( boxes_.n > 0 && !drawer_ );
    if ( boxes_.n > 0 )
    {
        if ( drawer_ )
        {
            drawer_->draw_slot( frame.tensor_idx, boxes_.d.data(), boxes_.n );
            drawer_->sync_cpu();
        }
        else
        {
            dma::DmaSync wr( tn.fd, true );
            dma::cache_drop( tn.data, tn.size );
            const int row_stride = static_cast<int>( tn.stride_bytes > 0 ? tn.stride_bytes : stride_bytes_ );
            draw_boxes_cpu( static_cast<uint8_t*>( tn.data ),
                            model_w_, model_h_, row_stride, boxes_.d.data(), boxes_.n );
            dma::cache_clean( tn.data, tn.size );
        }
    }

    if ( prev_live )
    {
        const auto t5 = clk::now();
        dma::DmaSync rd( tn.fd, false );
        // Whenever CPU draw did not invalidate cache lines this frame (GPU draw OR
        // --cpu-draw with 0 boxes), drop stale lines before CPU reads RGA DRAM output.
        if ( !cpu_drew )
            dma::cache_drop( tn.data, tn.size );
        preview_->publish( static_cast<const uint8_t*>( tn.data ) );
        enc_ms += ms( t5, clk::now() );
    }
    if ( enc_live )
    {
        const auto t5 = clk::now();
        enc_->publish( tn.fd, frame.lb, frame.ts_us, frame.src_idx );
        const auto t6 = clk::now();
        enc_ms = ms( t5, t6 );
    }
    return enc_ms;
}
