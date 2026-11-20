#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "overlay_sink.hpp"
#include "box_mapper.h"
#include "config/config.hpp"  // StreamConfig
#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "core/overlay_raster.hpp"  // draw_boxes_cpu, draw_callouts_cpu
#include "core/rga_limits.hpp"  // rgb888_stride_px
#include "core/status.hpp"
#include "dma_sync.hpp"
#include "timings.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

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
    label_scale_ = stream.label_scale;
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
        log_.info() << "net: padded input tensor rows (" << stride_bytes_ << " bytes, packed " << fallback_stride
                    << "): every consumer reads the stride\n";

    if ( stream.h264 )
    {
        EncoderConfig ec = encoder_config( stream, model_w_, model_h_, primary_band, nsrc, log_ );
        ec.model_stride_px = rga_limits::rgb888_stride_px( stride_bytes_, model_w_ );
        layout_ = ec.layout;
        tiled_ = true;
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
    track_filt_[src_idx].clear();
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

// Output tile px per tensor px for this frame's band: the h264 stream scales the
// band into its tile (place_tile); the raw NUT preview ships the tensor 1:1.
float OverlaySink::tile_scale( const FrameView& frame ) const noexcept
{
    if ( !tiled_ )
        return 1.0f;
    const TilePlacement tp = place_tile( layout_, frame.lb, frame.src_idx );
    return tp.src.h > 0 && tp.dst.h > 0 ? static_cast<float>( tp.dst.h ) / static_cast<float>( tp.src.h ) : 1.0f;
}

bool OverlaySink::draw_overlay_into_tensor( const FrameView& frame,
                                            const DetList& dets,
                                            const TrajList& trajs,
                                            const Tracker& tracker ) noexcept
{
    const float sx = frame.lb.w / static_cast<float>( frame.src_w );
    const float sy = frame.lb.h / static_cast<float>( frame.src_h );
    // disp: the display copy of the tracks, boxes through BoxFilter; trajs keeps
    // the posteriors the trail was recorded from (BoxMapper bends the trail's head
    // onto the filtered box).
    TrajList disp = trajs;
    const bool valid_src = frame.src_idx >= 0 && frame.src_idx < reid_cfg::kMaxSources;
    if ( valid_src )
        track_filt_[frame.src_idx].apply( disp, frame.ts_us, box_filter::kTrackBoxParams );
    boxes_.clear();
    mapper_.fill( dets, disp, trajs, tracker, boxes_, frame.lb, sx, sy, frame.ts_us );
    callouts_.clear();
    const CalloutConfig ccfg { label_lang_ == LabelLang::Ru, callout_text_scale( label_scale_, tile_scale( frame ) ) };
    if ( valid_src )
        compute_callouts( disp, frame.lb, sx, sy, ccfg, callout_hist_[frame.src_idx], callouts_ );
    else
        compute_callouts( disp, frame.lb, sx, sy, ccfg, callouts_ );
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
    const dma::DmaSync wr( tn.fd, true );
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
        const dma::DmaSync rd( tn.fd, false );
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
