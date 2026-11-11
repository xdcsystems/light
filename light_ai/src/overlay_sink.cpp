#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <unistd.h>

#include "overlay_sink.hpp"
#include "box_mapper.h"
#include "config/cli.hpp"
#include "dma_sync.hpp"
#include "rknn_engine.hpp"
#include "slot_ring.hpp"
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
        auto clamp = []( int v, int hi ) {
            return v < 0 ? 0 : ( v > hi ? hi : v );
        };
        for ( int i = 0; i < n; ++i )
        {
            const GpuBox& b = boxes[i];
            if ( b.x1 <= 0.0f || b.x0 >= static_cast<float>( w ) || b.y1 <= 0.0f
                 || b.y0 >= static_cast<float>( h ) )
                continue;
            if ( b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f )
                continue;
            const int x0 = clamp( static_cast<int>( std::floor( b.x0 ) ), w - 1 );
            const int y0 = clamp( static_cast<int>( std::floor( b.y0 ) ), h - 1 );
            const int x1 = std::max( x0, clamp( static_cast<int>( std::ceil( b.x1 ) ) - 1, w - 1 ) );
            const int y1 = std::max( y0, clamp( static_cast<int>( std::ceil( b.y1 ) ) - 1, h - 1 ) );
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
            for ( int x = x0; x <= x1; ++x )
            {
                put( x, y0 );
                put( x, y1 );
            }
            for ( int y = y0; y <= y1; ++y )
            {
                put( x0, y );
                put( x1, y );
            }
        }
    }

}  // namespace

void OverlaySink::init( const Config& cfg,
                        const std::vector<RknnEngine>& npus,
                        int model_w,
                        int model_h,
                        const LbGeom& primary_band,
                        int nsrc )
{
    model_w_ = model_w;
    model_h_ = model_h;
    if ( !cfg.net )
        return;

    const rknn_tensor_attr& ia = npus[0].input_attr();
    std::cout << "net: tensor stride check: size_with_stride=" << ia.size_with_stride
              << " vs " << model_w_ * model_h_ * 3 << "\n";

    if ( cfg.net_h264 )
    {
        enc_.emplace( model_w_, model_h_, primary_band, cfg.net_fps, cfg.net_port, cfg.net_host, cfg.net_bitrate, nsrc );
        if ( cfg.net_host.empty() )
            std::cout << "net: h264 PULL server on port " << cfg.net_port << ", " << cfg.net_bitrate
                      << " kbps (viewer: ffplay tcp://<board-ip>:" << cfg.net_port << ")\n";
        else
            std::cout << "net: h264 preview -> " << cfg.net_host << ':' << cfg.net_port
                      << ", " << cfg.net_bitrate << " kbps\n";
    }
    else
    {
        preview_.emplace( model_w_, model_h_, model_w_ * 3, cfg.net_fps, cfg.net_port );
        std::cout << "net: preview server on port " << cfg.net_port << "\n";
    }

    init_gpu_drawer( cfg, npus );
}

void OverlaySink::init_gpu_drawer( const Config& cfg, const std::vector<RknnEngine>& npus )
{
    if ( ( !preview_ && !enc_ ) || cfg.cpu_draw )
        return;

    drawer_.emplace();

    bool drew_ok = drawer_->init();
    const int npool = static_cast<int>( npus.size() );
    for ( int k = 0; drew_ok && k < npool; ++k )
        drew_ok = drawer_->attach_slot( k, npus[k].input_fd(), model_w_, model_h_,
                static_cast<uint32_t>( model_w_ ) * 3 );

    if ( !drew_ok )
    {
        std::cerr << "pipeline: gpu drawer unavailable, CPU fallback\n";
        drawer_.reset();
    }
    else
    {
        std::cout << "net: gpu box drawing active (Mali G610, EGL/GBM)\n";
    }
}

double OverlaySink::draw_and_publish( Slot& sl,
                                      int ps,
                                      const DetList& dets,
                                      const TrajList& trajs,
                                      const Tracker& tracker )
{
    double enc_ms = 0.0;
    const bool enc_live = enc_ && enc_->viewer_active();
    const bool prev_live = preview_ && preview_->viewer_connected();
    if ( !enc_live && !prev_live )
        return enc_ms;

    const float sx = sl.lb.w / static_cast<float>( sl.src_w );
    const float sy = sl.lb.h / static_cast<float>( sl.src_h );
    boxes_.clear();
    fill_gpu_boxes( dets, trajs, tracker, boxes_, sl.lb, sx, sy, sl.ts_us );
    if ( boxes_.n > 0 )
    {
        if ( drawer_ )
        {
            drawer_->draw_slot( ps, boxes_.d.data(), boxes_.n );
            drawer_->sync_cpu();
        }
        else
        {
            dma::DmaSync wr( sl.eng->input_fd(), true );
            dma::cache_drop( sl.eng->input_data(), sl.eng->input_size() );
            draw_boxes_cpu( static_cast<uint8_t*>( sl.eng->input_data() ),
                            model_w_, model_h_, model_w_ * 3, boxes_.d.data(), boxes_.n );
            dma::cache_clean( sl.eng->input_data(), sl.eng->input_size() );
        }
    }

    if ( prev_live )
    {
        const auto t5 = clk::now();
        dma::DmaSync rd( sl.eng->input_fd(), false );
        if ( drawer_ )
            dma::cache_drop( sl.eng->input_data(), sl.eng->input_size() );
        preview_->publish( static_cast<const uint8_t*>( sl.eng->input_data() ) );
        enc_ms += ms( t5, clk::now() );
    }
    if ( enc_live )
    {
        const auto t5 = clk::now();
        enc_->publish( sl.eng->input_fd(), sl.lb, -1, sl.ts_us, sl.src_idx );
        const auto t6 = clk::now();
        enc_ms = ms( t5, t6 );
    }
    return enc_ms;
}
