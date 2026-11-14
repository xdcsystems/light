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
                uint8_t* px = rgb + static_cast<size_t>( y ) * stride + x * 3;
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
    if ( !stream.enabled )
        return;

    // The input tensor is allocated with size_with_stride bytes when the
    // runtime reports one, else size (rknn_engine.cpp): a mismatch with the
    // packed RGB888 size means padded rows.
    log_.info() << "net: tensor stride check: input mem=" << tensors.d[0].size
                << " vs " << model_w_ * model_h_ * 3 << "\n";

    if ( stream.h264 )
    {
        enc_.emplace( log_, model_w_, model_h_, primary_band, stream.fps, stream.port, stream.host, stream.bitrate_kbps, nsrc );
        if ( stream.host.empty() )
            log_.info() << "net: h264 PULL server on port " << stream.port << ", " << stream.bitrate_kbps
                      << " kbps (viewer: ffplay tcp://<board-ip>:" << stream.port << ")\n";
        else
            log_.info() << "net: h264 preview -> " << stream.host << ':' << stream.port
                      << ", " << stream.bitrate_kbps << " kbps\n";
    }
    else
    {
        preview_.emplace( model_w_, model_h_, model_w_ * 3, stream.fps, stream.port );
        log_.info() << "net: preview server on port " << stream.port << "\n";
    }

    init_gpu_drawer( stream.cpu_draw, tensors );
}

// NO SIGNAL idle publish: h264 path only -- the encoder RGA-copies the
// pre-rendered gray plate into the absent tiles. The raw NUT preview
// keeps its live-only diagnostic role (no idle frames there).
void OverlaySink::publish_idle( int64_t ts_us, const bool* absent, int nabs )
{
    if ( enc_ )
        enc_->publish_idle( ts_us, absent, nabs );
}

void OverlaySink::init_gpu_drawer( bool cpu_draw, const TensorSet& tensors )
{
    if ( ( !preview_ && !enc_ ) || cpu_draw )
        return;

    drawer_.emplace();

    bool drew_ok = drawer_->init();
    for ( int k = 0; drew_ok && k < tensors.n; ++k )
        drew_ok = drawer_->attach_slot( k, tensors.d[k].fd, model_w_, model_h_,
                static_cast<uint32_t>( model_w_ ) * 3 );

    if ( !drew_ok )
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
                                      const Tracker& tracker )
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
    fill_gpu_boxes( dets, trajs, tracker, boxes_, frame.lb, sx, sy, frame.ts_us );
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
            draw_boxes_cpu( static_cast<uint8_t*>( tn.data ),
                            model_w_, model_h_, model_w_ * 3, boxes_.d.data(), boxes_.n );
            dma::cache_clean( tn.data, tn.size );
        }
    }

    if ( prev_live )
    {
        const auto t5 = clk::now();
        dma::DmaSync rd( tn.fd, false );
        if ( drawer_ )
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
