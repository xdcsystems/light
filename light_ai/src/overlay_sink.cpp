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
#include "core/glyph_font.hpp"
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

    void blend_px( uint8_t* px, uint8_t r, uint8_t g, uint8_t b, unsigned a ) noexcept
    {
        if ( a >= 255u )
        {
            px[0] = r;
            px[1] = g;
            px[2] = b;
            return;
        }
        px[0] = static_cast<uint8_t>( ( r * a + px[0] * ( 255u - a ) + 127u ) / 255u );
        px[1] = static_cast<uint8_t>( ( g * a + px[1] * ( 255u - a ) + 127u ) / 255u );
        px[2] = static_cast<uint8_t>( ( b * a + px[2] * ( 255u - a ) + 127u ) / 255u );
    }

    void fill_rect_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutRect& rect,
                        uint8_t r, uint8_t g, uint8_t b, unsigned a ) noexcept
    {
        const int x0 = std::max( rect.x0, 0 );
        const int y0 = std::max( rect.y0, 0 );
        const int x1 = std::min( rect.x1, w );
        const int y1 = std::min( rect.y1, h );
        for ( int y = y0; y < y1; ++y )
        {
            uint8_t* row = rgb + static_cast<size_t>( y ) * stride;
            for ( int x = x0; x < x1; ++x )
                blend_px( row + static_cast<std::ptrdiff_t>( x ) * 3, r, g, b, a );
        }
    }

    void draw_glyph_cpu( uint8_t* rgb, int w, int h, int stride, int gx, int gy, int glyph_idx,
                         uint8_t r, uint8_t g, uint8_t b ) noexcept
    {
        const uint8_t* alpha = glyph_font::glyph_alpha( glyph_idx );
        for ( int pr = 0; pr < glyph_font::kGlyphH; ++pr )
        {
            const int py = gy + pr;
            if ( py < 0 || py >= h )
                continue;
            const uint8_t* row = alpha + pr * glyph_font::kGlyphW;
            uint8_t* line = rgb + static_cast<size_t>( py ) * stride;
            for ( int pc = 0; pc < glyph_font::kGlyphW; ++pc )
            {
                const unsigned a = row[pc];
                if ( a == 0u )
                    continue;
                const int px = gx + pc;
                if ( px >= 0 && px < w )
                    blend_px( line + static_cast<std::ptrdiff_t>( px ) * 3, r, g, b, a );
            }
        }
    }

    void draw_callouts_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutList& list ) noexcept
    {
        for ( int i = 0; i < list.n; ++i )
        {
            const CalloutItem& c = list.d[i];
            for ( int s = 0; s < c.n_segs; ++s )
                fill_rect_cpu( rgb, w, h, stride, c.segs[s], c.r, c.g, c.b, 255u );

            // pill fill/border removed (PR3): only connector + text are drawn

            int gx = c.text_x;
            int pos = 0;
            while ( pos < c.text_bytes )
            {
                const uint32_t cp = glyph_font::next_utf8( c.text, c.text_bytes, pos );
                if ( cp == 0 )
                    break;
                draw_glyph_cpu( rgb, w, h, stride, gx, c.text_y, glyph_font::glyph_index( cp ), 255, 255, 255 );
                gx += CalloutItem::kGlyphW;
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
    label_lang_ = stream.label_lang;
    model_w_ = model_w;
    model_h_ = model_h;
    const uint32_t fallback_stride = static_cast<uint32_t>( model_w_ ) * 3;
    stride_bytes_ = ( tensors.n > 0 && tensors.d[0].stride_bytes > 0 ) ? tensors.d[0].stride_bytes : fallback_stride;
    if ( !stream.enabled )
        return;

    // Compare allocated mem against stride_bytes * model_h_ (avoiding tn.size / model_h_
    // which overestimates when native model input is FP16).
    const uint64_t expected_bytes = static_cast<uint64_t>( stride_bytes_ ) * static_cast<uint64_t>( model_h_ );
    const size_t t0_size = tensors.n > 0 ? tensors.d[0].size : 0;
    log_.info() << "net: tensor stride check: input mem=" << t0_size
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
void OverlaySink::reset_source( int src_idx ) noexcept
{
    if ( src_idx < 0 || src_idx >= reid_cfg::kMaxSources )
        return;
    callout_hist_[src_idx].entries.clear();
    track_stab_[src_idx].clear();
}

void OverlaySink::reset_all_sources() noexcept
{
    for ( int s = 0; s < reid_cfg::kMaxSources; ++s )
        reset_source( s );
}

void OverlaySink::sync_viewer_session() noexcept
{
    const bool enc_open = enc_ && enc_->viewer_active();
    const bool prev_open = preview_ && preview_->session_open();
    const bool any_open = enc_open || prev_open;
    const uint32_t enc_ep = enc_ ? enc_->session_epoch() : 0;
    const uint32_t prev_ep = preview_ ? preview_->session_epoch() : 0;

    if ( !any_open )
    {
        if ( was_viewer_live_ )
        {
            reset_all_sources();
            was_viewer_live_ = false;
        }
        return;
    }

    const bool ep_changed = ( enc_open && enc_ep != last_enc_epoch_ ) ||
                            ( prev_open && prev_ep != last_prev_epoch_ );
    if ( !was_viewer_live_ || ep_changed )
        reset_all_sources();

    was_viewer_live_ = true;
    if ( enc_open )
        last_enc_epoch_ = enc_ep;
    if ( prev_open )
        last_prev_epoch_ = prev_ep;
}

void OverlaySink::publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept
{
    sync_viewer_session();
    if ( enc_ )
        enc_->publish_idle( ts_us, absent, nabs );
}

void OverlaySink::flush_dirty() noexcept
{
    sync_viewer_session();
    if ( enc_ )
        enc_->flush_dirty();
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

int OverlaySink::find_stab_idx( const TrackStabList& list, int id, uint32_t gen ) const noexcept
{
    const auto* const end = list.d.begin() + list.n;
    const auto* const it = std::find_if( list.d.begin(), end, [id, gen]( const TrackDispStab& e ) {
        return e.id == id && e.gen == gen;
    } );
    return it != end ? static_cast<int>( it - list.d.begin() ) : -1;
}

void OverlaySink::stabilize_display_tracks( int src_idx, TrajList& disp ) noexcept
{
    TrackStabList& prev = track_stab_[src_idx];
    TrackStabList next {};
    for ( int k = 0; k < disp.n; ++k )
    {
        TrackState& tr = disp.d[k];
        if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
            continue;
        const float x0 = tr.cx - tr.bw * 0.5f;
        const float y0 = tr.cy - tr.bh * 0.5f;
        const float x1 = tr.cx + tr.bw * 0.5f;
        const float y1 = tr.cy + tr.bh * 0.5f;
        const int idx = find_stab_idx( prev, tr.id, tr.gen );
        TrackDispStab entry {};
        entry.id = tr.id;
        entry.gen = tr.gen;
        if ( idx >= 0 )
        {
            entry.env = prev.d[idx].env;
            entry.env.step( x0, y0, x1, y1, 0.0f );
        }
        else
        {
            entry.env.reset( x0, y0, x1, y1 );
        }
        tr.cx = entry.env.cx();
        tr.cy = entry.env.cy();
        tr.bw = entry.env.bw();
        tr.bh = entry.env.bh();
        next.push( entry );
    }
    prev = next;
}

bool OverlaySink::draw_overlay_into_tensor( const FrameView& frame,
                                            const DetList& dets,
                                            const TrajList& trajs,
                                            const Tracker& tracker ) noexcept
{
    const float sx = frame.lb.w / static_cast<float>( frame.src_w );
    const float sy = frame.lb.h / static_cast<float>( frame.src_h );
    TrajList disp = trajs;
    const bool valid_src = frame.src_idx >= 0 && frame.src_idx < reid_cfg::kMaxSources;
    if ( valid_src )
        stabilize_display_tracks( frame.src_idx, disp );
    boxes_.clear();
    mapper_.fill( dets, disp, tracker, boxes_, frame.lb, sx, sy, frame.ts_us );
    callouts_.clear();
    if ( valid_src )
        compute_callouts( disp, frame.lb, sx, sy, { label_lang_ == LabelLang::Ru }, callout_hist_[frame.src_idx], callouts_ );
    else
        compute_callouts( disp, frame.lb, sx, sy, { label_lang_ == LabelLang::Ru }, callouts_ );
    if ( boxes_.n <= 0 && callouts_.n <= 0 )
        return false;

    if ( drawer_ )
    {
        const auto tg0 = clk::now();
        drawer_->draw_slot( frame.tensor_idx, boxes_.d.data(), boxes_.n, &callouts_ );
        const auto tg1 = clk::now();
        drawer_->sync_cpu();
        const auto tg2 = clk::now();
        perf_.gpu_draw_ms_max = std::max( perf_.gpu_draw_ms_max, ms( tg0, tg1 ) );
        perf_.sync_cpu_ms_max = std::max( perf_.sync_cpu_ms_max, ms( tg1, tg2 ) );
        return false;
    }
    const TensorRef& tn = frame.tensor;
    if ( tn.data == nullptr )
        return false;
    dma::DmaSync wr( tn.fd, true );
    dma::cache_drop( tn.data, tn.size );
    const int row_stride = static_cast<int>( tn.stride_bytes > 0 ? tn.stride_bytes : stride_bytes_ );
    uint8_t* rgb = static_cast<uint8_t*>( tn.data );
    draw_boxes_cpu( rgb, model_w_, model_h_, row_stride, boxes_.d.data(), boxes_.n );
    draw_callouts_cpu( rgb, model_w_, model_h_, row_stride, callouts_ );
    dma::cache_clean( tn.data, tn.size );
    return true;
}

double OverlaySink::draw_and_publish( const FrameView& frame,
                                      const DetList& dets,
                                      const TrajList& trajs,
                                      const Tracker& tracker,
                                      bool defer_encode ) noexcept
{
    sync_viewer_session();
    double enc_ms = 0.0;
    const bool enc_live = enc_ && enc_->viewer_active();
    const bool prev_live = preview_ && preview_->viewer_connected();
    if ( !enc_live && !prev_live )
        return enc_ms;

    const TensorRef& tn = frame.tensor;
    const bool cpu_drew = draw_overlay_into_tensor( frame, dets, trajs, tracker );

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
        enc_->publish( tn.fd, frame.lb, frame.ts_us, frame.src_idx, defer_encode );
        const auto t6 = clk::now();
        enc_ms = ms( t5, t6 );
    }
    return enc_ms;
}

OverlayPerfWindow OverlaySink::take_perf() noexcept
{
    if ( drawer_ )
    {
        const uint64_t ft = drawer_->fence_timeouts();
        perf_.fence_tmo_cnt = static_cast<uint32_t>( ft - prev_fence_tmo_ );
        prev_fence_tmo_ = ft;
    }
    const OverlayPerfWindow out = perf_;
    perf_.reset();
    return out;
}

EncPerfWindow OverlaySink::take_enc_perf() noexcept
{
    return enc_ ? enc_->take_perf() : EncPerfWindow {};
}
