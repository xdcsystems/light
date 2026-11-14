#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <thread>

#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "cpu_affinity.hpp"
#include "dma_sync.hpp"
#include "inference_pool.hpp"
#include "pipeline.hpp"
#include "timings.hpp"
#include "yolo_postprocess.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    // M5: per-source box-log path: with several sources "boxes.csv"
    // becomes "boxes_s1.csv" (suffix before the extension; no dot -> append).
    std::string source_box_log( const std::string& path, int idx, int nsrc )
    {
        if ( path.empty() || nsrc <= 1 )
            return path;
        const size_t dot = path.rfind( '.' );
        const std::string suf = "_s" + std::to_string( idx );
        return dot == std::string::npos ? path + suf : path.substr( 0, dot ) + suf + path.substr( dot );
    }

}  // namespace

Pipeline::Pipeline( const Config& cfg, const StopFlag& stop, const Log& log )
  : cfg_( cfg )
  , locator_( usb_host_, log )
  , srcs_ { { SourceChannel( log ), SourceChannel( log ), SourceChannel( log ) } }
  , pool_( cfg_.infer, YoloDecode( cfg_.infer.conf_thr, cfg_.infer.iou_thr ), log, PoolOptions {},
          cfg_.infer.model.c_str() )
  , t_( log )
  , sink_( log )
  , stop_( stop )
  , log_( log )
{
    static_assert( kMaxSources == 3, "srcs_ initializer lists one SourceChannel per source" );
    // pool_ is up by now: the engines are loaded, their I/O is set up,
    // they are pinned to NPU cores and the workers wait for frames. If
    // anything below throws, pool_'s destructor joins the workers -- the
    // ctor can no longer leave joinable threads behind.
    t_.set_pool( pool_.size() );

    // Single source of truth for the model geometry: the model file itself
    // (RknnEngine::input_width/height). All engines share the model.
    const rknn_tensor_attr& ia = pool_.engine( 0 ).input_attr();
    if ( ia.n_dims < 4 )
    {
        log_.warn() << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
        throw std::runtime_error( "rknn: input tensor is not 4-D NHWC" );
    }

    // Gray letterbox bars on every pooled tensor: RGA writes only the
    // visible band (drect), so the 128-fill done here survives every frame.
    for ( int k = 0; k < pool_.size(); ++k )
    {
        const TensorRef tn = pool_.tensor( k );
        dma::fill_and_flush( tn.fd, tn.data, 128, tn.size );
    }

    model_w_ = pool_.engine( 0 ).input_width();
    model_h_ = pool_.engine( 0 ).input_height();
    cal_dumper_.init( cfg_.cal, model_w_, model_h_ );

    // Channels know their kind, node and the model geometry; no device is
    // opened yet (attach_sources, last).
    init_source();

    // Canonical letterbox band: 4:3 of the model input. The canvas is
    // source-independent -- absent sources show the NO SIGNAL idle
    // plate (idle_tick), real bands land per-frame via prep_frame's
    // band_geom.
    const LbGeom enc_lb = band_geom( model_w_, model_h_ * 3 / 4, model_w_, model_h_ );
    sink_.init( cfg_.stream, pool_.tensors(), model_w_, model_h_, enc_lb, nsrc_ );

    if ( cfg_.cal.frames > 0 && cfg_.cal.dir.empty() )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
    }

    // Last, so run() starts at the live edge: the camera STREAMON and the net
    // socket drain happen after the encoder and the GPU drawer are up.
    attach_sources();
}

void Pipeline::init_source()
{
    const std::vector<SourceEndpoint>& eps = cfg_.sources;
    if ( eps.size() > static_cast<size_t>( kMaxSources ) )
        throw std::runtime_error( "pipeline: at most 3 sources are supported (one per NPU core)" );

    nsrc_ = static_cast<int>( eps.size() );
    // an empty source list is a legitimate waiting-for-attach run (README);
    // the sleep loop itself lands with the udev patch
    if ( nsrc_ == 0 )
        log_.info() << "sources configured: none - waiting for attach\n";
    // M5: per-source decoders -- mixed resolutions (1280x960 USB next to
    // 1280x720 net) are supported.
    for ( int s = 0; s < nsrc_; ++s )
        srcs_[s].configure( s, eps[static_cast<size_t>( s )], model_w_, model_h_ );
}

// The first attach of every channel, through the same SourceChannel::attach
// as the prober's. A source that is not there stays non-Live and the prober
// takes it (2 s cadence); a decoder or aux failure is fatal at startup, as
// before. No retry ladder here: retries are the prober's.
void Pipeline::attach_sources()
{
    using Attach = SourceChannel::Attach;
    bool any_aux = false;
    int max_w = 0;
    int max_h = 0;  // MAX across the started sources: the "aux not needed" line only
    for ( int s = 0; s < nsrc_; ++s )
    {
        SourceChannel& ch = srcs_[s];
        std::string why;
        const Attach a = ch.attach( busy_v4l2_nodes( srcs_, nsrc_, s ), locator_, false, why );
        if ( a == Attach::DecoderFailed )
            throw std::runtime_error( "mpp decoder init failed for " + ch.node() );
        if ( a == Attach::AuxFailed )
            throw std::runtime_error( "pipeline: aux buffer alloc failed for source " + ch.node() );
        if ( a != Attach::Live )
        {
            log_.warn() << "source s" << s << ": not started at startup (" << why << ") - prober retries every 2 s\n";
            continue;
        }
        max_w = std::max( max_w, ch.geom().w );
        max_h = std::max( max_h, ch.geom().h );
        any_aux = any_aux || ch.geom().aux_fd >= 0;
    }
    if ( !any_aux )
        log_.info() << "aux not needed: max " << max_w << 'x' << max_h
                    << " fits single-pass into " << model_w_ << 'x' << model_h_ << '\n';
}

// NO SIGNAL idle publish: while at least one configured source is
// absent, absent tiles show the gray NO SIGNAL plate in the stream.
// Paced like --net-fps (25 FPS in follow-source mode); idle encodes
// are NOT counted in --frames or the timing stats and never touch
// the NPU. A fully live composition skips the work entirely.
void Pipeline::idle_tick()
{
    if ( !sink_.enabled() )
        return;
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0_ ).count();
    const int64_t step_us = cfg_.stream.fps > 0 ? 1000000LL / cfg_.stream.fps : 40000;
    if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
        return;
    bool absent[ kMaxSources ] = {};
    bool any_absent = nsrc_ == 0;  // no configured sources: one gray canvas
    for ( int s = 0; s < nsrc_; ++s )
    {
        absent[ s ] = !srcs_[ s ].live();
        any_absent = any_absent || absent[ s ];
    }
    if ( !any_absent )
        return;
    last_idle_us_ = now_us;
    sink_.publish_idle( now_us, absent, nsrc_ );
}

FrameStatus Pipeline::try_dequeue_round( int wait_ms, EncodedFrame& f )
{
    constexpr double k_stall_ms = 2000.0;
    idle_tick();  // mixed state: absent tiles keep publishing NO SIGNAL
    for ( int k = 0; k < nsrc_; ++k )
    {
        const int s = ( cur_src_ + 1 + k ) % nsrc_;
        SourceChannel& ch = srcs_[s];
        if ( !ch.live() )
            continue;
        const FrameStatus st = ch.poll( wait_ms, f );
        if ( st != FrameStatus::NotReady )
        {
            cur_src_ = s;
            return st;
        }
        if ( ms( ch.last_ok(), clk::now() ) >= k_stall_ms )
        {
            log_.warn() << "source s" << s << ": no frame in " << static_cast<int>( k_stall_ms )
                        << " ms - treating as recoverable stall\n";
            cur_src_ = s;
            f.err = EIO;
            return FrameStatus::Detached;
        }
    }
    return FrameStatus::NotReady;
}

// Publishes every result that is already finished, in order; never waits.
void Pipeline::drain_done_slots( int& ok )
{
    while ( pool_.front_done() && !stop_.requested() )
        ok += publish_oldest();
}

FrameStatus Pipeline::dequeue_next_source( EncodedFrame& f, int& ok )
{
    drain_done_slots( ok );
    idle_tick();
    if ( nsrc_ == 0 )
    {
        // waiting for attach: nothing to dequeue, park until stop
        // (hot-adding sources beyond the configured set is a separate feature)
        while ( !stop_.requested() )
        {
            idle_tick();
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
        }
        return FrameStatus::Stopped;
    }
    if ( nsrc_ <= 1 && pool_.empty() )
    {
        cur_src_ = 0;
        while ( !stop_.requested() )
        {
            if ( !srcs_[0].live() )
            {
                // all-absent park: NO SIGNAL idle publish + 40 ms quanta;
                // the SourceProber attaches the source
                idle_tick();
                std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
                continue;
            }
            const FrameStatus st = srcs_[0].poll( -1, f );
            if ( st != FrameStatus::NotReady )
                return st;
        }
        return FrameStatus::Stopped;
    }
    while ( !stop_.requested() )
    {
        drain_done_slots( ok );
        bool any_live = false;
        for ( int s = 0; s < nsrc_; ++s )
            any_live = any_live || srcs_[s].live();
        if ( !any_live )
        {
            // all-absent park: NO SIGNAL idle publish + 40 ms quanta;
            // the SourceProber attaches the sources
            idle_tick();
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
            continue;
        }
        FrameStatus st = try_dequeue_round( 0, f );
        if ( st != FrameStatus::NotReady )
            return st;
        drain_done_slots( ok );
        st = try_dequeue_round( 1, f );
        if ( st != FrameStatus::NotReady )
            return st;
    }
    return FrameStatus::Stopped;
}

int Pipeline::run()
{
    int ok = 0;
    const auto t_start = clk::now();
    t0_ = std::chrono::steady_clock::now();  // idle pts base: same timeline as live ts_us
    // Hot-attach prober (README): a local, so its destructor stops and joins
    // it on ANY exit of run() -- drain, fatal error, exception unwind.
    SourceProber prober( srcs_, nsrc_, locator_, log_ );
    prober.start();
    // Spawn order: the prober was spawned BEFORE this pin, so it keeps the
    // scheduler default, as intended (cpu_affinity.hpp).
    pin_current_thread_to_big_cores( "pipeline main", log_ );

    for ( int s = 0; s < nsrc_; ++s )
    {
        an_[s].smoother.set_log( source_box_log( cfg_.box_log, s, nsrc_ ) );
        srcs_[s].stamp_ok( t_start );
    }
    // frames == 0 (the default): no frame budget -- run until stop
    // (Ctrl-C); the signal path below drains and reports as usual.
    for ( int i = 0; ( cfg_.frames <= 0 || i < cfg_.frames ) && !stop_.requested(); )
    {
        const auto t0 = clk::now();

        EncodedFrame f {};
        const FrameStatus st = dequeue_next_source( f, ok );
        if ( st != FrameStatus::Frame )
        {
            if ( st == FrameStatus::Stopped || stop_.requested() )
                break;
            if ( handle_source_failure( f ) != 0 )
                return -1;
            const auto now = clk::now();
            for ( int s = 0; s < nsrc_; ++s )
                srcs_[s].stamp_ok( now );
            continue;
        }
        SourceChannel& ch = srcs_[f.src];
        const auto t1 = clk::now();

        // Decode + requeue + warm-up (SourceChannel::decode): -1 for an
        // empty packet, a decode error or a warm-up frame. Such a frame
        // contributes neither to ok nor to the timing statistics -- otherwise
        // rare errors would silently skew averages.
        const int fd_src = ch.decode( f );
        const auto t2 = clk::now();
        if ( fd_src < 0 )
            continue;

        // ---- unified path: every frame goes through the InferencePool ----
        // pool 1 == ring of one: dispatch, then publish immediately --
        // the pipeline thread blocks until the worker is done, exactly the
        // legacy synchronous semantics. pool 3 keeps publish-before-reuse:
        // with the FIFO full, the oldest frame is published first.
        const double cam_ms = ms( t0, t1 );
        const double dec_ms = ms( t1, t2 );
        drain_done_slots( ok );
        if ( pool_.full() )
            ok += publish_oldest();
        const int slot = acquire_slot( ok );
        if ( slot < 0 )
            break;  // stop requested with every slot in flight: leave through the normal path
        const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t1 - t_start ).count();
        frame_dispatch( slot, f.src, an_[f.src].frames++, fd_src, ts_us, cam_ms, dec_ms );
        ++i;
        if ( pool_.size() == 1 )
            ok += publish_oldest();
    }

    if ( stop_.requested() )
        log_.info() << "stop: signal received, draining and flushing...\n";

    // M4: drain the ring -- publish whatever the workers already finished.
    while ( !pool_.empty() )
        ok += publish_oldest();

    const double wall = std::chrono::duration<double>( clk::now() - t_start ).count();
    t_.report_total();
    rep_.summary( ok, wall, reconnects_ );
    return ok;
}

// Disconnect policy: ENODEV/EIO mean the source is gone (unplug, USB
// hiccup, Wi-Fi drop) - the channel is detached and the SourceProber owns
// the re-attach on its 2 s cadence (SourceChannel::attach, the same
// path as at startup). Any other errno is a hard error and ends the run
// with exit code 1 (run() returns -1, main() maps it to 1). The net
// source reports only these two classes by contract. The recovery
// MECHANISM is the channel's attach(); the POLICY (cadence) is the
// prober's. The old inline 20x1 s ladder is gone: it froze the frame
// loop and starved any other live source for up to 20 s.

int Pipeline::handle_source_failure( const EncodedFrame& f )
{
    const bool recoverable = ( f.err == ENODEV || f.err == EIO );
    log_.warn() << "source: dequeue failed: " << strerror( f.err )
                << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
    if ( !recoverable )
        return -1;
    // M4: frames from the dead stream are never published - drop the
    // pending slots (their infer may still be in flight; the pool waits
    // for it, then frees without publishing). A no-op with one engine:
    // its single frame was published before the next dequeue.
    pool_.discard( f.src );
    ++reconnects_;
    srcs_[f.src].detach();  // the prober owns the channel from here (SourceChannel thread rule)
    log_.info() << "source s" << f.src << ": detached - probe_loop will re-attach\n";
    return 0;
}

// Claim-or-drain: a Free slot is NOT guaranteed here. After a give-up
// on a wedged NPU the abandoned slot stays in flight until its own
// worker finishes the rknn_run -- only that worker may free it (freeing
// here would race the NPU tensor read; InferSlot). So: ask the pool for
// a Free slot; if every slot is in flight, publish the oldest pending
// result when there is one, otherwise sleep briefly and ask again (a
// rknn_run away from finishing, ~30 ms). The stop_ check keeps Ctrl-C
// from parking here forever on a wedged NPU. Returns the claimed slot
// index, or -1 when stop was requested with every slot in flight.

int Pipeline::acquire_slot( int& ok )
{
    bool starved_warned = false;
    while ( true )
    {
        const int k = pool_.try_acquire();
        if ( k >= 0 )
            return k;
        if ( stop_.requested() )
            return -1;
        if ( !starved_warned )
        {
            log_.warn() << "pipeline: all " << pool_.size() << " slots in flight, waiting for a worker to release one\n";
            starved_warned = true;
        }
        if ( !pool_.empty() )
            ok += publish_oldest();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

// RGA resize into the slot's tensor, optional cal dump, one-time
// cross-check of the RGA letterbox geometry against the single-owner
// one, then the job goes to the pool.

void Pipeline::frame_dispatch( int slot, int src, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms )
{
    const TensorRef tn = pool_.tensor( slot );
    LbGeom& band = tensor_band_[static_cast<size_t>( slot )];
    const auto t3 = clk::now();

    // Stale-line drop before the RGA rewrite: see cache_drop().
    // Full tensor: draws and preview reads from previous frames
    // may have left resident lines anywhere in it.

    // tn.data != nullptr: the InferencePool constructor throws when setup_io()
    // fails, so a live slot always has its tensor mapped; the check states the
    // invariant for the analyzer, like the null guard inside cache_drop().
    const SourceChannel& ch = srcs_[src];
    const SourceGeom& ss = ch.geom();  // Live: the frame thread owns it
    if ( tn.data != nullptr && band.w > 0
         && ( band.left != ss.lb.left || band.top != ss.lb.top || band.w != ss.lb.w || band.h != ss.lb.h ) )
    {
        dma::DmaSync wr( tn.fd, true );
        std::memset( tn.data, 128, tn.size );
    }
    dma::cache_drop( tn.data, tn.size );
    LbGeom lg {};
    const bool rga_ok =
            rga_.resize( fd_src, ss.w, ss.h, tn.fd, model_w_, model_h_, ss.aux_fd, ss.w / 2, ss.h / 2, &lg,
                    ch.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1 );
    cal_dumper_.maybe( frame_idx, tn.fd, static_cast<const uint8_t*>( tn.data ), tn.size );
    const auto t3b = clk::now();
    // Cross-check RGA geometry against the owning source's (warn once).
    if ( !lb_warned_ && ( lg.left != ss.lb.left || lg.top != ss.lb.top || lg.w != ss.lb.w || lg.h != ss.lb.h ) )
    {
        lb_warned_ = true;
        log_.warn() << "pipeline: RGA letterbox drift (src " << src << "): x=" << lg.left << " top=" << lg.top
                    << " w=" << lg.w << " h=" << lg.h
                    << " vs x=" << ss.lb.left << " top=" << ss.lb.top << " w=" << ss.lb.w << " h=" << ss.lb.h << '\n';
    }

    InferJob job {};
    job.src_idx = src;
    job.src_w = ss.w;
    job.src_h = ss.h;
    job.frame = frame_idx;
    job.ts_us = ts_us;
    job.rga_ok = rga_ok;
    job.lb = ss.lb;
    job.tr.cam = cam_ms;
    job.tr.dec = dec_ms;
    job.tr.rga = ms( t3, t3b );
    band = ss.lb;
    static_cast<void>( pool_.submit( slot, job ) );  // false: logged by the pool, frame dropped
}

// collect -> analytics -> output. The result is copied out of the pool and
// its slot is Free again before the overlay touches the tensor: that is
// safe because only this thread refills a tensor (frame_dispatch), and it
// does so only after this frame is fully drawn and published.
int Pipeline::publish_oldest()
{
    if ( !pool_.collect( res_ ) )
        return 0;  // gave up on a wedged NPU (logged by the pool): frame dropped

    double enc_ms = 0.0;
    if ( res_.npu_ok )
    {
        // M3.10: EMA smoothing on scene-coord boxes; the raw list is kept
        // only for the --box-log jitter A/B CSV.

        SourceAnalytics& ss = an_[res_.job.src_idx];
        ss.smoother.update( res_.dets, dets_ );
        ss.smoother.log_frame( res_.job.frame, res_.dets, dets_ );
        // Anti-jitter stage 1: the Kalman tracker consumes RAW person
        // detections. Feeding it the EMA-smoothed dets_ double-filtered
        // the signal: the filter velocity lagged the (already damped)
        // positions and the rendered box trailed real motion on every
        // direction change.
        // dets_ stays the BoxSmoother output -- it still feeds the
        // non-person drawing below and the --box-log CSV.
        ss.tracker.update( res_.dets, res_.job.ts_us, trajs_ );

        rep_.report( res_.job.frame, dets_, trajs_, res_.job.src_idx, nsrc_ );
        if ( sink_.enabled() )
        {
            FrameView view {};
            view.tensor_idx = res_.slot;
            view.tensor = pool_.tensor( res_.slot );
            view.lb = res_.job.lb;
            view.src_idx = res_.job.src_idx;
            view.src_w = res_.job.src_w;
            view.src_h = res_.job.src_h;
            view.ts_us = res_.job.ts_us;
            enc_ms = sink_.draw_and_publish( view, dets_, trajs_, ss.tracker );
        }
    }

    res_.job.tr.enc = enc_ms;
    t_.add( res_.job.tr );
    return res_.npu_ok ? 1 : 0;
}
