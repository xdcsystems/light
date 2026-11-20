#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <thread>

#include "core/fixed_fmt.hpp"
#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "core/rga_limits.hpp"
#include "core/status.hpp"
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
  , srcs_( log, [&log]( int /*idx*/ ) { return SourceChannel( log ); } )
  , rga_( log )
  , pool_( cfg_.infer, YoloDecode( cfg_.infer.conf_thr, cfg_.infer.iou_thr, log ), log, PoolOptions {},
          cfg_.infer.model.c_str(), log )
  , cal_dumper_( log )
  , t_( log )
  , sink_( log )
  , rep_( log )
  , stop_( stop )
  , log_( log )
{
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

    // check_yolo_heads: the model outputs must be a 3-head yolov5 layout --
    // one head per stride 8/16/32, ch == 255 -- before any frame runs. A
    // violation is fatal here: decode would only warn and skip the head, and
    // the run would show no detections at all.
    {
        const auto& outs = pool_.engine( 0 ).output_attrs();
        int seen[3] = { 0, 0, 0 };
        for ( size_t gi = 0; gi < outs.size(); ++gi )
        {
            const rknn_tensor_attr& attr = outs[gi];
            if ( attr.n_dims != 4 )
            {
                log_.warn() << "pipeline: unexpected output tensor ndims=" << attr.n_dims << '\n';
                throw std::runtime_error( "rknn: output tensor " + std::to_string( gi ) + " is not 4-D NCHW" );
            }
            const YoloHead probe { nullptr, static_cast<int>( attr.dims[1] ), static_cast<int>( attr.dims[2] ),
                                   static_cast<int>( attr.dims[3] ) };
            int stride = 0;
            int head = -1;
            if ( !yolo_head_shape( probe, static_cast<int>( gi ), model_w_, model_h_, log_, stride, head ) )
                throw std::runtime_error( "rknn: output head " + std::to_string( gi ) + " is not a yolov5 head" );
            if ( attr.dims[1] != 255 )  // 3 anchors x (4 box + 1 obj + 80 classes)
                throw std::runtime_error( "rknn: output head " + std::to_string( gi ) + " ch " +
                                          std::to_string( attr.dims[1] ) + ", expected 255" );
            ++seen[head];
        }
        if ( outs.size() != 3 || seen[0] != 1 || seen[1] != 1 || seen[2] != 1 )
            throw std::runtime_error( "rknn: expected 3 yolov5 heads (one per stride 8/16/32), got " +
                                      std::to_string( outs.size() ) );
    }

    cal_dumper_.init( cfg_.cal, model_w_, model_h_, pool_.engine( 0 ).input_stride_bytes() );

    // Channels know their kind, node and the model geometry; no device is
    // opened yet (attach_sources, last).
    srcs_.configure( cfg_.sources, model_w_, model_h_ );
    srcs_.set_stop( &stop_.raw() );

    // Canonical letterbox band: 4:3 of the model input. The canvas is
    // source-independent -- absent sources show the NO SIGNAL idle
    // plate (idle_tick), real bands land per-frame via prep_frame's
    // band_geom.
    const LbGeom enc_lb = band_geom( model_w_, model_h_ * 3 / 4, model_w_, model_h_ );
    sink_.init( cfg_.stream, pool_.tensors(), model_w_, model_h_, enc_lb, srcs_.size() );

    global_tracks_.set_sim_thr( cfg_.infer.reid_thr );
    if ( !cfg_.infer.reid_model.empty() )
        reid_worker_.emplace( cfg_.infer.reid_model.c_str(), pool_.size(), cfg_.infer.core, log_ );

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
    for ( int s = 0; s < srcs_.size(); ++s )
    {
        SourceChannel& ch = srcs_[s];
        Status why;
        const Attach a = ch.attach( srcs_.busy_nodes( s ), locator_, false, why );
        if ( a == Attach::DecoderFailed )
            throw_on( why, "mpp decoder init failed for " + ch.node() );
        if ( a == Attach::AuxFailed )
            throw_on( why, "pipeline: aux buffer alloc failed for source " + ch.node() );
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
// absent, absent tiles show the gray NO SIGNAL plate in the stream --
// with a viewer only: without one NetEncoder::publish_idle returns at once.
// Paced like --net-fps (25 FPS in follow-source mode); idle encodes
// are NOT counted in --frames or the timing stats and never touch
// the NPU. A fully live composition skips the work entirely.
void Pipeline::idle_tick() noexcept
{
    if ( !sink_.enabled() )
        return;
    sink_.flush_dirty();
    maybe_log_perf( clk::now() );
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0_ ).count();
    const int64_t step_us = cfg_.stream.fps > 0 ? 1000000LL / cfg_.stream.fps : 40000;
    if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
        return;
    std::array<bool, kMaxSources> absent {};
    const bool any_absent = srcs_.absent_mask( absent );
    if ( !any_absent && srcs_.size() > 0 )  // no configured sources: one gray canvas
        return;
    last_idle_us_ = now_us;
    sink_.publish_idle( now_us, absent.data(), srcs_.size() );
}

// Publishes every result that is already finished, in order; never waits.
void Pipeline::drain_done_slots( int& ok ) noexcept
{
    while ( pool_.front_done() && !stop_.requested() )
        ok += publish_oldest();
    if ( sink_.enabled() )
        sink_.flush_dirty();
}

// The one park block: nothing to dequeue while no source is Live (none
// configured, or all absent) -- NO SIGNAL idle publish + 40 ms quanta, the
// SourceProber attaches the sources. One source with an empty pool (--pool 1)
// takes the blocking dequeue; otherwise two rounds per pass, the first without
// waiting, the second with 1 ms, finished results published in between.
FrameStatus Pipeline::next_frame( EncodedFrame& f, int& ok ) noexcept
{
    const bool blocking = srcs_.size() == 1 && pool_.empty();
    while ( !stop_.requested() )
    {
        drain_done_slots( ok );
        idle_tick();  // mixed state: absent tiles keep publishing NO SIGNAL
        if ( !srcs_.any_live() )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
            continue;
        }
        if ( pool_.size() > 1 && pool_.full() )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            continue;
        }
        FrameStatus st = blocking ? srcs_.next_blocking( f ) : srcs_.next( 0, f );
        if ( st != FrameStatus::NotReady )
            return st;
        if ( blocking )
            continue;
        drain_done_slots( ok );
        idle_tick();
        st = srcs_.next( 1, f );
        if ( st != FrameStatus::NotReady )
            return st;
    }
    return FrameStatus::Stopped;
}

// One frame through the loop. Decode + requeue + warm-up (SourceChannel::decode)
// give -1 for an empty packet, a decode error or a warm-up frame: such a frame
// counts neither in ok nor in the timing statistics -- otherwise rare errors
// would silently skew the averages.
Pipeline::Step Pipeline::step( int& ok, Status& st ) noexcept
{
    st = Status {};
    const auto t0 = clk::now();
    EncodedFrame f {};
    const FrameStatus fs = next_frame( f, ok );
    if ( fs != FrameStatus::Frame )
    {
        if ( fs == FrameStatus::Stopped || stop_.requested() )
            return Step::Stop;
        if ( const Status hst = handle_source_failure( f ); !hst.ok() )
        {
            st = hst;
            return Step::Fatal;
        }
        finish_step_perf( t0 );
        return Step::Skipped;
    }
    const auto t1 = clk::now();
    record_frame_perf( f.src, t1 );
    Status dec_st;
    const int fd_src = srcs_[f.src].decode( f, &dec_st );
    const auto t2 = clk::now();
    if ( cfg_.stream.enabled && f.src >= 0 && f.src < kMaxSources )
        src_perf_[f.src].dec_ms_max = std::max( src_perf_[f.src].dec_ms_max, ms( t1, t2 ) );
    if ( fd_src < 0 )
    {
        if ( !dec_st.ok() )
        {
            EncodedFrame df = f;
            df.st = dec_st;
            if ( const Status hst = handle_source_failure( df ); !hst.ok() )
            {
                st = hst;
                return Step::Fatal;
            }
        }
        finish_step_perf( t0 );
        return Step::Skipped;
    }

    // ---- unified path: every frame goes through the InferencePool ----
    // pool 1 == ring of one: dispatch, then publish immediately -- the
    // pipeline thread blocks until the worker is done, exactly the legacy
    // synchronous semantics. pool 3 keeps publish-before-reuse: with the FIFO
    // full, the oldest frame is published first.
    update_auto_orient( f.src, fd_src, ok );
    if ( stop_.requested() )
        return Step::Stop;
    drain_done_slots( ok );
    if ( pool_.full() )
        ok += publish_oldest();
    const int slot = acquire_slot( ok );
    if ( slot < 0 )
        return Step::Stop;  // stop requested with every slot in flight: leave through the normal path
    const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0_ ).count();
    frame_dispatch( slot, f.src, an_[f.src].frames++, fd_src, ts_us, ms( t0, t1 ), ms( t1, t2 ) );
    if ( pool_.size() == 1 )
        ok += publish_oldest();
    finish_step_perf( t0 );
    return Step::Dispatched;
}

Status Pipeline::run()
{
    int ok = 0;
    t0_ = clk::now();
    perf_win_start_ = t0_;
    // Hot-attach prober (README): a local, so its destructor stops and joins
    // it on ANY exit of run() -- drain, fatal error, exception unwind. Spawned
    // BEFORE the pin below: it keeps the scheduler default (cpu_affinity.hpp).
    SourceProber prober( srcs_, locator_, log_ );
    prober.start();
    pin_current_thread_to_big_cores( "pipeline main", log_ );
    for ( int s = 0; s < srcs_.size(); ++s )
        an_[s].smoother.set_log( source_box_log( cfg_.box_log, s, srcs_.size() ), log_ );
    srcs_.reset_stall_clocks( t0_ );
    // frames == 0 (no --frames, the default): run until stop (Ctrl-C), then drain and report.
    for ( int i = 0; ( cfg_.frames <= 0 || i < cfg_.frames ) && !stop_.requested(); )
    {
        Status fst;
        const Step r = step( ok, fst );
        if ( r == Step::Fatal )
            return fst;
        if ( r == Step::Stop )
            break;
        i += r == Step::Dispatched ? 1 : 0;
    }
    if ( stop_.requested() )
        log_.info() << "stop: signal received, draining and flushing...\n";
    while ( !pool_.empty() )  // M4: drain the ring -- publish what the workers already finished
        ok += publish_oldest();
    t_.report_total();
    rep_.summary( ok, std::chrono::duration<double>( clk::now() - t0_ ).count(), reconnects_ );
    return {};
}

// Disconnect policy: ENODEV/EIO mean the source is gone (unplug, USB
// hiccup, Wi-Fi drop) - the channel is detached and the SourceProber owns
// the re-attach on its 2 s cadence (SourceChannel::attach, the same
// path as at startup). Any other errno is a hard error and ends the run
// with exit code 1 (run() returns the failure Status, main() maps it
// to 1). The net
// source reports only these two classes by contract. The recovery
// MECHANISM is the channel's attach(); the POLICY (cadence) is the
// prober's. The old inline 20x1 s ladder is gone: it froze the frame
// loop and starved any other live source for up to 20 s.

void Pipeline::reset_source_analytics( int src ) noexcept
{
    if ( src < 0 || src >= kMaxSources )
        return;
    an_[src].tracker.reset();
    an_[src].smoother.reset();
    trajs_.clear();
    const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( clk::now() - t0_ ).count();
    global_tracks_.sync_cam( src, trajs_, ts_us );
    sink_.reset_source( src );
    has_last_frame_[src] = false;
}

Status Pipeline::handle_source_failure( const EncodedFrame& f ) noexcept
{
    const bool recoverable = f.st.code() == Errc::Lost;  // ENODEV / EIO (dequeue_status)
    log_.warn() << "source: " << f.st << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
    if ( !recoverable )
        return f.st;
    // M4: frames from the dead stream are never published - drop the
    // pending slots immediately (any in-flight infer is abandoned and freed
    // by its worker upon completion). A no-op with one engine:
    // its single frame was published before the next dequeue.
    pool_.discard( f.src );
    ++reconnects_;
    srcs_[f.src].detach();  // the prober owns the channel from here (SourceChannel thread rule)
    reset_source_analytics( f.src );
    log_.info() << "source s" << f.src << ": detached - the prober will re-attach\n";
    return {};
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

int Pipeline::acquire_slot( int& ok ) noexcept
{
    bool starved_warned = false;
    int k = pool_.try_acquire();
    while ( k < 0 && !stop_.requested() )
    {
        if ( !starved_warned )
        {
            log_.warn() << "pipeline: all " << pool_.size() << " slots in flight, waiting for a worker to release one\n";
            starved_warned = true;
        }
        if ( !pool_.empty() )
            ok += publish_oldest();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        k = pool_.try_acquire();
    }
    return k;  // try_acquire(): a slot index or -1
}

// RGA resize into the slot's tensor, optional cal dump, one-time
// cross-check of the RGA letterbox geometry against the single-owner
// one, then the job goes to the pool.

void Pipeline::prepare_tensor_band( int slot, const LbGeom& eff_lb ) noexcept
{
    const TensorRef tn = pool_.tensor( slot );
    LbGeom& band = tensor_band_[static_cast<size_t>( slot )];
    if ( tn.data != nullptr && band.w > 0 &&
         ( band.left != eff_lb.left || band.top != eff_lb.top || band.w != eff_lb.w || band.h != eff_lb.h ) )
    {
        dma::fill_and_flush( tn.fd, tn.data, 128, tn.size );
        band = {};
    }
}

bool Pipeline::submit_orient_probe( int src, int fd_src, int rot, int job_idx, int& ok ) noexcept
{
    const int slot = acquire_slot( ok );
    if ( slot < 0 )
        return false;
    const SourceChannel& ch = srcs_[src];
    const SourceGeom& ss = ch.geom();
    const int raw_w = ss.raw_w > 0 ? ss.raw_w : ss.w;
    const int raw_h = ss.raw_h > 0 ? ss.raw_h : ss.h;
    int ew = 0;
    int eh = 0;
    auto_orient::oriented_dims( raw_w, raw_h, rot, ew, eh );
    const LbGeom elb = band_geom( ew, eh, model_w_, model_h_ );
    prepare_tensor_band( slot, elb );

    const TensorRef tn = pool_.tensor( slot );
    LbGeom lg {};
    const int yuv_fmt = ( ch.out_fmt() == MPP_FMT_YUV420SP ) ? 0 : 1;
    const Status rga_st = rga_.resize( fd_src, raw_w, raw_h, tn.fd, model_w_, model_h_, ss.aux_fd,
                                       rga_limits::aux_side( raw_w ), rga_limits::aux_side( raw_h ),
                                       &lg, yuv_fmt, rot, ch.hor_stride(), ch.ver_stride(),
                                       rga_limits::rgb888_stride_px( tn.stride_bytes, model_w_ ) );
    if ( rga_st.ok() )
        tensor_band_[static_cast<size_t>( slot )] = elb;
    const InferJob job { src, ew, eh, job_idx, -1, rga_st.ok(), elb, {} };
    return pool_.submit( slot, job );
}

bool Pipeline::probe_rot_batch( int src, int fd_src, const int* rots, int n_rots, float* scores, int& ok ) noexcept
{
    const auto pt0 = clk::now();
    while ( !pool_.empty() && !stop_.requested() )
        ok += publish_oldest();
    if ( stop_.requested() )
    {
        record_orient_perf( src, ms( pt0, clk::now() ) );
        return false;
    }

    for ( int off = 0; off < n_rots && !stop_.requested(); )
    {
        const int batch = std::min( pool_.size(), n_rots - off );
        int sent = 0;
        for ( int b = 0; b < batch; ++b )
        {
            if ( submit_orient_probe( src, fd_src, rots[off + b], off + b, ok ) )
                ++sent;
        }
        for ( int c = 0; c < sent; ++c )
        {
            InferResult pr {};
            if ( pool_.collect( pr ) && pr.npu_ok && pr.job.frame >= 0 && pr.job.frame < n_rots )
                scores[pr.job.frame] = auto_orient::score_detections( pr.dets );
        }
        off += batch;
    }
    record_orient_perf( src, ms( pt0, clk::now() ) );
    return !stop_.requested();
}

void Pipeline::update_auto_orient( int src, int fd_src, int& ok ) noexcept
{
    const SourceChannel& ch = srcs_[src];
    SourceAnalytics& sa = an_[src];
    const uint32_t seq = ch.attach_seq();
    if ( seq != sa.seen_attach_seq )
    {
        const SourceGeom& ss = ch.geom();
        sa.orient.reset( ch.rot_auto(), ch.rot_deg(), ss.raw_h > ss.raw_w );
        sa.seen_attach_seq = seq;
        sa.cur_rot = sa.orient.active_rot();
        reset_source_analytics( src );
    }
    if ( !sa.orient.enabled() || sa.orient.locked() )
        return;
    const int exif_rot = ch.exif_rot();
    if ( exif_rot >= 0 )
    {
        sa.orient.set_exif_hint( exif_rot );
        sa.cur_rot = sa.orient.active_rot();
    }

    const AutoOrient::Action act = sa.orient.next_action( sa.frames );
    if ( act == AutoOrient::Action::None )
        return;
    if ( act == AutoOrient::Action::ProbeSingle )
    {
        const int single_rot = sa.orient.watch_angle();
        float single_score = 0.0f;
        if ( !probe_rot_batch( src, fd_src, &single_rot, 1, &single_score, ok ) ||
             !sa.orient.observe_single( single_rot, single_score ) )
            return;
    }

    constexpr std::array<int, 4> rots { 0, 90, 180, 270 };
    std::array<float, 4> scores {};
    if ( !probe_rot_batch( src, fd_src, rots.data(), 4, scores.data(), ok ) )
        return;
    const AutoOrient::Result res = sa.orient.commit_all4( scores.data() );
    sa.cur_rot = res.rot_deg;
    if ( res.changed )
        reset_source_analytics( src );
    if ( res.changed || res.locked )
    {
        log_.info() << "source s" << src << ": auto-rot " << res.rot_deg << " deg (score="
                    << res.best_score << " vs " << res.second_score
                    << ( res.locked ? ", locked)\n" : ", voting)\n" );
    }
}

void Pipeline::frame_dispatch( int slot, int src, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms ) noexcept
{
    const auto t3 = clk::now();
    const SourceChannel& ch = srcs_[src];
    const SourceGeom& ss = ch.geom();  // Live: the frame thread owns it
    const int rot = an_[src].cur_rot;
    const int raw_w = ss.raw_w > 0 ? ss.raw_w : ss.w;
    const int raw_h = ss.raw_h > 0 ? ss.raw_h : ss.h;
    int eff_w = 0;
    int eff_h = 0;
    auto_orient::oriented_dims( raw_w, raw_h, rot, eff_w, eff_h );
    const LbGeom eff_lb = ( rot == ss.rot_deg ) ? ss.lb : band_geom( eff_w, eff_h, model_w_, model_h_ );
    prepare_tensor_band( slot, eff_lb );

    const TensorRef tn = pool_.tensor( slot );
    LbGeom lg {};
    const Status rga_st =
            rga_.resize( fd_src, raw_w, raw_h, tn.fd, model_w_, model_h_, ss.aux_fd,
                         rga_limits::aux_side( raw_w ), rga_limits::aux_side( raw_h ), &lg,
                         ch.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1, rot, ch.hor_stride(), ch.ver_stride(),
                         rga_limits::rgb888_stride_px( tn.stride_bytes, model_w_ ) );
    if ( rga_st.ok() )
    {
        cal_dumper_.maybe( frame_idx, tn.fd, static_cast<const uint8_t*>( tn.data ), tn.size );
        // Cross-check RGA geometry against the owning source's (warn once).
        if ( !lb_warned_ && ( lg.left != eff_lb.left || lg.top != eff_lb.top || lg.w != eff_lb.w || lg.h != eff_lb.h ) )
        {
            lb_warned_ = true;
            log_.warn() << "pipeline: RGA letterbox drift (src " << src << "): x=" << lg.left << " top=" << lg.top
                        << " w=" << lg.w << " h=" << lg.h
                        << " vs x=" << eff_lb.left << " top=" << eff_lb.top << " w=" << eff_lb.w << " h=" << eff_lb.h << '\n';
        }
        tensor_band_[static_cast<size_t>( slot )] = eff_lb;
    }
    const auto t3b = clk::now();

    InferJob job {};
    job.src_idx = src;
    job.src_w = eff_w;
    job.src_h = eff_h;
    job.frame = frame_idx;
    job.ts_us = ts_us;
    job.rga_ok = rga_st.ok();
    job.lb = eff_lb;
    job.tr.cam = cam_ms;
    job.tr.dec = dec_ms;
    job.tr.rga = ms( t3, t3b );
    static_cast<void>( pool_.submit( slot, job ) );  // false: logged by the pool, frame dropped
}

// collect -> analytics -> output. The result is copied out of the pool and
// its slot is Free again before the overlay touches the tensor: that is
// safe because only this thread refills a tensor (frame_dispatch), and it
// does so only after this frame is fully drawn and published.
int Pipeline::publish_oldest() noexcept
{
    if ( !pool_.collect( res_ ) )
        return 0;  // gave up on a wedged NPU (logged by the pool): frame dropped

    double enc_ms = 0.0;
    if ( res_.npu_ok )
    {
        // BoxSmoother: the BoxFilter display filter (One-Euro + dead zone,
        // core/box_filter.hpp) of the non-person scene-coord boxes; the raw
        // list is kept only for the --box-log jitter A/B CSV.

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

        FrameView view {};
        view.tensor_idx = res_.slot;
        view.tensor = pool_.tensor( res_.slot );
        view.lb = res_.job.lb;
        view.src_idx = res_.job.src_idx;
        view.src_w = res_.job.src_w;
        view.src_h = res_.job.src_h;
        view.ts_us = res_.job.ts_us;
        if ( reid_worker_ )
            run_reid_stage( view );

        rep_.report( res_.job.frame, dets_, trajs_, res_.job.src_idx, srcs_.size() );
        const bool defer_enc = srcs_.size() > 1 && pool_.front_done();
        if ( sink_.enabled() )
            enc_ms = sink_.draw_and_publish( view, dets_, trajs_, ss.tracker, defer_enc );
    }

    res_.job.tr.enc = enc_ms;
    t_.add( res_.job.tr, res_.npu_ok );
    return res_.npu_ok ? 1 : 0;
}

void Pipeline::run_reid_stage( const FrameView& view ) noexcept
{
    ReidResult done {};
    while ( reid_worker_->poll_result( done ) )
        global_tracks_.apply_result( done );

    global_tracks_.sync_cam( view.src_idx, trajs_, view.ts_us );
    if ( !res_.job.rga_ok || view.tensor.fd < 0 )
        return;

    std::array<ReidCropReq, reid_cfg::kMaxCropsPerFrame> reqs {};
    const int rw = reid_worker_->input_width();
    const int rh = reid_worker_->input_height();
    const int nr = global_tracks_.select_crops( view.src_idx, trajs_, view.lb, view.src_w, view.src_h,
                                                rw, rh, reqs.data(), reid_cfg::kMaxCropsPerFrame );
    for ( int i = 0; i < nr; ++i )
    {
        const int rslot = reid_worker_->try_acquire_slot();
        if ( rslot < 0 )
            break;
        const TensorRef dst = reid_worker_->input_tensor( rslot );
        if ( dst.fd < 0 )
            break;
        const RgaResizer::CropImage src_img { view.tensor.fd, model_w_, model_h_,
                                              rga_limits::rgb888_stride_px( view.tensor.stride_bytes, model_w_ ) };
        const RgaResizer::CropImage dst_img { dst.fd, rw, rh, rga_limits::rgb888_stride_px( dst.stride_bytes, rw ) };
        if ( const Status cst = rga_.crop_resize( src_img, reqs[i].srect, dst_img ); !cst.ok() )
        {
            // A failed crop only skips this Re-ID sample (the slot stays free),
            // but a persistent cause must be visible: first failure, then every 256th.
            if ( reid_crop_fail_++ % 256 == 0 )
                log_.warn() << "reid: crop " << reqs[i].srect.w << 'x' << reqs[i].srect.h << " failed: " << cst
                            << " (" << reid_crop_fail_ << " so far)\n";
            continue;
        }
        const ReidJob job { rslot, view.src_idx, reqs[i].local_id, reqs[i].gen, view.ts_us };
        if ( reid_worker_->submit( job ) )
            global_tracks_.mark_dispatched( view.src_idx, reqs[i].local_id, reqs[i].gen );
    }
}

void Pipeline::record_frame_perf( int src, std::chrono::steady_clock::time_point tp ) noexcept
{
    if ( !cfg_.stream.enabled || src < 0 || src >= kMaxSources )
        return;
    SourcePerfWindow& sp = src_perf_[src];
    if ( has_last_frame_[src] )
        sp.max_dq_gap_ms = std::max( sp.max_dq_gap_ms, ms( last_frame_tp_[src], tp ) );
    last_frame_tp_[src] = tp;
    has_last_frame_[src] = true;
    ++sp.frames;
}

void Pipeline::record_orient_perf( int src, double elapsed_ms ) noexcept
{
    if ( !cfg_.stream.enabled || src < 0 || src >= kMaxSources )
        return;
    SourcePerfWindow& sp = src_perf_[src];
    ++sp.orient_probe_cnt;
    sp.orient_probe_ms_max = std::max( sp.orient_probe_ms_max, elapsed_ms );
}

void Pipeline::finish_step_perf( std::chrono::steady_clock::time_point t0 ) noexcept
{
    if ( !cfg_.stream.enabled )
        return;
    const auto now = clk::now();
    step_ms_max_ = std::max( step_ms_max_, ms( t0, now ) );
    maybe_log_perf( now );
}

void Pipeline::maybe_log_perf( std::chrono::steady_clock::time_point now ) noexcept
{
    if ( !cfg_.stream.enabled || ms( perf_win_start_, now ) < 5000.0 )
        return;
    log_src_perf();
    log_enc_perf();
    perf_win_start_ = now;
}

void Pipeline::log_src_perf() noexcept
{
    Log::Line line = log_.info();
    line << "[perf] src step_max=" << Fixed { step_ms_max_, 1 };
    step_ms_max_ = 0.0;
    for ( int s = 0; s < srcs_.size(); ++s )
    {
        SourcePerfWindow& sp = src_perf_[s];
        sp.eagain_cnt = srcs_.take_eagain_cnt( s );
        const uint64_t skip_now = srcs_[s].skip_parts_cnt();
        sp.skip_parts_cnt = static_cast<uint32_t>( skip_now - prev_skip_parts_[s] );
        prev_skip_parts_[s] = skip_now;
        line << " | s" << s << " f=" << sp.frames << " dq_max=" << Fixed { sp.max_dq_gap_ms, 1 }
             << " eagain=" << sp.eagain_cnt << " dec_max=" << Fixed { sp.dec_ms_max, 1 }
             << " or_cnt=" << sp.orient_probe_cnt << " or_max=" << Fixed { sp.orient_probe_ms_max, 1 }
             << " skip=" << sp.skip_parts_cnt;
        sp.reset();
    }
    line << '\n';
}

void Pipeline::log_enc_perf() noexcept
{
    const OverlayPerfWindow op = sink_.take_perf();
    const EncPerfWindow ep = sink_.take_enc_perf();
    Log::Line line = log_.info();
    if ( sink_.has_enc_perf() )
    {
        line << "[perf] enc admit=" << ep.enc_admit << " skip=" << ep.enc_skip
             << " idle_a=" << ep.idle_admit << " idle_s=" << ep.idle_skip
             << " dt_max=" << Fixed { ep.max_dt_enc_ms, 1 }
             << " dt_hist=[" << ep.dt_enc_hist[0] << ',' << ep.dt_enc_hist[1] << ','
             << ep.dt_enc_hist[2] << ',' << ep.dt_enc_hist[3] << ','
             << ep.dt_enc_hist[4] << ',' << ep.dt_enc_hist[5] << ']'
             << " h264_max=" << Fixed { ep.h264_ms_max, 1 }
             << " cmp_max=" << Fixed { ep.compose_ms_max, 1 }
             << " eagain=" << ep.eagain_cnt << " clamp=" << ep.pts_clamp_cnt
             << " qflush=" << ep.q_flush_cnt << " | ";
    }
    else
    {
        line << "[perf] ";
    }
    line << "ovl gpu_max=" << Fixed { op.gpu_draw_ms_max, 1 }
         << " sync_max=" << Fixed { op.sync_cpu_ms_max, 1 }
         << " ftmo=" << op.fence_tmo_cnt << '\n';
}
