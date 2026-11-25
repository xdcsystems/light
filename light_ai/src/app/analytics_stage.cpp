#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "app/analytics_stage.hpp"
#include "core/rga_limits.hpp"

namespace
{
    // Per-source box-log path: with several sources "boxes.csv" becomes
    // "boxes_s1.csv" (suffix before the extension; no dot -> append).
    std::string source_box_log( const std::string& path, int idx, int nsrc )
    {
        if ( path.empty() || nsrc <= 1 )
            return path;
        const size_t dot = path.rfind( '.' );
        const std::string suf = "_s" + std::to_string( idx );
        return dot == std::string::npos ? path + suf : path.substr( 0, dot ) + suf + path.substr( dot );
    }
}  // namespace

AnalyticsStage::AnalyticsStage( const Log& log ) : log_( log ) {}

void AnalyticsStage::init( const InferConfig& cfg, int model_w, int model_h, int pool_size )
{
    model_w_ = model_w;
    model_h_ = model_h;
    global_tracks_.set_sim_thr( cfg.reid_thr );
    if ( !cfg.reid_model.empty() )
        start_reid( cfg, pool_size );
}

void AnalyticsStage::init_box_logs( const std::string& box_log, int nsrc )
{
    nsrc_ = std::clamp( nsrc, 1, static_cast<int>( kMaxSources ) );
    for ( int s = 0; s < nsrc; ++s )
        an_[s].smoother.set_log( source_box_log( box_log, s, nsrc ), log_ );
}

bool AnalyticsStage::sync_attach_seq( int src, uint32_t seq ) noexcept
{
    const auto idx = static_cast<size_t>( src );
    if ( seq == an_[idx].seen_attach_seq )
        return false;
    an_[idx].seen_attach_seq = seq;
    return true;
}

// Re-ID crops the detector tensor in one RGA pass (run_reid_stage passes no
// aux buffer): a detector input over 8x the Re-ID input on a side would fail
// every crop, so Re-ID stays off and recognition runs on without it.
void AnalyticsStage::start_reid( const InferConfig& cfg, int pool_size )
{
    reid_worker_.emplace( cfg.reid_model.c_str(), pool_size, cfg.core, log_ );
    const int rw = reid_worker_->input_width();
    const int rh = reid_worker_->input_height();
    if ( !rga_limits::reid_crop_needs_aux( model_w_, model_h_, rw, rh ) )
        return;
    log_.error() << "reid: detector input " << model_w_ << 'x' << model_h_ << " is over " << rga_limits::kMaxRatio
                 << "x the Re-ID input " << rw << 'x' << rh
                 << " on a side (the one-pass RGA crop limit) - Re-ID off, worker released\n";
    reid_worker_.reset();
}

void AnalyticsStage::process( const InferResult& res, const FrameView& view, const RgaResizer& rga ) noexcept
{
    SourceAnalytics& ss = an_[static_cast<size_t>( res.job.src_idx )];
    ss.smoother.update( res.dets, dets_, res.job.ts_us );
    ss.smoother.log_frame( res.job.frame, res.dets, dets_ );
    // Anti-jitter stage 1: the Kalman tracker consumes RAW person detections;
    // dets_ stays the BoxSmoother output for non-person drawing and --box-log.
    ss.tracker.update( res.dets, res.job.ts_us, trajs_ );
    if ( reid_worker_ )
        run_reid_stage( view, res.job.rga_ok, rga );
}

void AnalyticsStage::reset_source_analytics( int src, int64_t ts_us ) noexcept
{
    if ( src < 0 || src >= kMaxSources )
        return;
    const auto idx = static_cast<size_t>( src );
    an_[idx].tracker.reset();
    an_[idx].smoother.reset();
    trajs_.clear();
    global_tracks_.sync_cam( src, trajs_, ts_us );
}

void AnalyticsStage::drain_reid_results() noexcept
{
    ReidResult done {};
    while ( reid_worker_->poll_result( done ) )
    {
        if ( global_tracks_.apply_result( done ) )
            ++reid_perf_.matches;
    }
}

void AnalyticsStage::run_reid_stage( const FrameView& view, bool rga_ok, const RgaResizer& rga ) noexcept
{
    if ( view.src_idx < 0 || view.src_idx >= kMaxSources )
        return;
    // Sync current frame first so tracks that died on this frame cannot match
    // late Re-ID results and spawn 45s phantom global tracks.
    global_tracks_.sync_cam( view.src_idx, trajs_, view.ts_us );
    drain_reid_results();

    global_tracks_.refresh_global_ids( view.src_idx, trajs_ );
    if ( !rga_ok || view.tensor.fd < 0 )
        return;

    std::array<ReidCropReq, reid_cfg::kMaxCropsPerFrame> reqs {};
    const int rw = reid_worker_->input_width();
    const int rh = reid_worker_->input_height();
    const int nr = global_tracks_.select_crops( view.src_idx, trajs_, view.lb, view.src_w, view.src_h, rw, rh,
                                                reqs.data(), reid_cfg::kMaxCropsPerFrame );
    const int cam_cap = ( nsrc_ > 1 ) ? std::max( 1, static_cast<int>( reid_cfg::kReidSlots ) / nsrc_ )
                                      : static_cast<int>( reid_cfg::kReidSlots );
    for ( int i = 0; i < nr; ++i )
    {
        if ( reid_worker_->inflight_for( view.src_idx ) >= cam_cap )
        {
            reid_perf_.drops += static_cast<uint32_t>( nr - i );
            break;
        }
        const int rslot = reid_worker_->try_acquire_slot();
        if ( rslot < 0 )
        {
            reid_perf_.drops += static_cast<uint32_t>( nr - i );
            break;
        }
        const TensorRef dst = reid_worker_->input_tensor( rslot );
        if ( dst.fd < 0 )
        {
            reid_perf_.drops += static_cast<uint32_t>( nr - i );
            break;
        }
        const RgaResizer::CropImage src_img { view.tensor.fd, model_w_, model_h_,
                                              rga_limits::rgb888_stride_px( view.tensor.stride_bytes, model_w_ ) };
        const RgaResizer::CropImage dst_img { dst.fd, rw, rh, rga_limits::rgb888_stride_px( dst.stride_bytes, rw ) };
        // No aux: start_reid() keeps Re-ID off when a crop could need the two-pass path.
        if ( const Status cst = rga.crop_resize( src_img, reqs[i].srect, dst_img, {} ); !cst.ok() )
        {
            ++reid_perf_.drops;
            if ( reid_crop_fail_++ % 256 == 0 )
                log_.warn() << "reid: crop " << reqs[i].srect.w << 'x' << reqs[i].srect.h << " failed: " << cst
                            << " (" << reid_crop_fail_ << " so far)\n";
            continue;
        }
        const ReidJob job { rslot, view.src_idx, reqs[i].local_id, reqs[i].gen, view.ts_us };
        if ( reid_worker_->submit( job ) )
        {
            ++reid_perf_.crops;
            global_tracks_.mark_dispatched( view.src_idx, reqs[i].local_id, reqs[i].gen );
        }
        else
        {
            ++reid_perf_.drops;
        }
    }
}

ReidPerfWindow AnalyticsStage::take_reid_perf() noexcept
{
    ReidPerfWindow out = reid_perf_;
    out.ms_max = reid_worker_ ? reid_worker_->take_max_ms() : 0.0;
    reid_perf_.reset();
    return out;
}
