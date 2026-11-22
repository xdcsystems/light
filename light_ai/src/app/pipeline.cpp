#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "app/pipeline.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/status.hpp"
#include "core/timings.hpp"
#include "npu/yolo_postprocess.hpp"
#include "sys/cpu_affinity.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    // Validates that the loaded model has a 4-D NHWC input and a 3-head
    // YOLOv5 output (one head per stride 8/16/32, ch == 255).
    void validate_yolo_model( const RknnEngine& eng, const Log& log )
    {
        const rknn_tensor_attr& ia = eng.input_attr();
        if ( ia.n_dims < 4 )
        {
            log.warn() << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
            throw std::runtime_error( "rknn: input tensor is not 4-D NHWC" );
        }
        const int model_w = eng.input_width();
        const int model_h = eng.input_height();
        const auto& outs = eng.output_attrs();
        int seen[3] = { 0, 0, 0 };
        for ( size_t gi = 0; gi < outs.size(); ++gi )
        {
            const rknn_tensor_attr& attr = outs[gi];
            if ( attr.n_dims != 4 )
            {
                log.warn() << "pipeline: unexpected output tensor ndims=" << attr.n_dims << '\n';
                throw std::runtime_error( "rknn: output tensor " + std::to_string( gi ) + " is not 4-D NCHW" );
            }
            const YoloHead probe { nullptr, static_cast<int>( attr.dims[1] ), static_cast<int>( attr.dims[2] ),
                                   static_cast<int>( attr.dims[3] ) };
            int stride = 0;
            int head = -1;
            if ( !yolo_head_shape( probe, static_cast<int>( gi ), model_w, model_h, log, stride, head ) )
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
}  // namespace

namespace
{
    void sample_rt_faults_if_due( PerfReporter& perf, PerfClock::time_point now ) noexcept
    {
        if ( !perf.window_due( now ) )
            return;
        uint64_t minflt = 0;
        uint64_t majflt = 0;
        read_thread_page_faults( minflt, majflt );
        perf.note_rt_faults( minflt, majflt );
    }
}  // namespace

Pipeline::Pipeline( const Config& cfg, const StopFlag& stop, const Log& log )
  : cfg_( cfg )
  , sources_( stop, log )
  , pool_( cfg_.infer, YoloDecode( cfg_.infer.conf_thr, cfg_.infer.iou_thr, log ), log, PoolOptions {},
           cfg_.infer.model.c_str(), log )
  , pre_( log )
  , analytics_( log )
  , output_( log )
  , t_( log )
  , stop_( stop )
  , log_( log )
  , perf_( cfg.stream.enabled, log )
{
    t_.set_pool( pool_.size() );

    const RknnEngine& eng = pool_.engine( 0 );
    validate_yolo_model( eng, log_ );
    const int model_w = eng.input_width();
    const int model_h = eng.input_height();

    const TensorSet tensors = pool_.tensors();
    pre_.init( cfg_.cal, model_w, model_h, eng.input_stride_bytes(), tensors );
    sources_.configure( cfg_.sources, model_w, model_h );

    const LbGeom enc_lb = band_geom( model_w, model_h * 3 / 4, model_w, model_h );
    output_.init( cfg_.stream, tensors, model_w, model_h, enc_lb, sources_.size() );
    analytics_.init( cfg_.infer, model_w, model_h, pool_.size() );

    // Last, so run() starts at the live edge: STREAMON / socket drain happen
    // after the encoder and the GPU drawer are up.
    sources_.attach_initial( model_w, model_h );
}

int64_t Pipeline::now_us() const noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>( clk::now() - t0_ ).count();
}

void Pipeline::reset_source( int src ) noexcept
{
    analytics_.reset_source_analytics( src, now_us() );
    output_.reset_source( src );
    perf_.source_reset( src );
}

void Pipeline::drain_done_slots( int& ok ) noexcept
{
    while ( pool_.front_done() && !stop_.requested() )
        ok += publish_oldest();
    output_.flush_dirty();
}

int Pipeline::acquire_slot( int& ok, LbGeom eff_lb ) noexcept
{
    bool starved_warned = false;
    int k = pool_.try_acquire( output_.busy_slot_mask(), eff_lb );
    while ( k < 0 && !stop_.requested() )
    {
        if ( !starved_warned )
        {
            log_.warn() << "pipeline: all " << pool_.size()
                        << " slots in flight, waiting for a worker to release one\n";
            starved_warned = true;
        }
        if ( !pool_.empty() )
            ok += publish_oldest();
        else
            static_cast<void>( pool_.wait_any_slot_free( std::chrono::milliseconds( 5 ) ) );
        k = pool_.try_acquire( output_.busy_slot_mask(), eff_lb );
    }
    if ( k >= 0 && !output_.wait_slot_free( k ) )
        return -1;
    return k;
}

void Pipeline::sync_and_orient( int src, int fd_src, int& ok ) noexcept
{
    const SourceChannel& ch = sources_[src];
    if ( analytics_.sync_attach_seq( src, ch.attach_seq() ) )
    {
        pre_.reset_orient( src, ch );
        reset_source( src );
    }
    const auto publish_fn = [this, &ok]() noexcept { ok += publish_oldest(); };
    const auto acquire_fn = [this, &ok]() noexcept { return acquire_slot( ok ); };
    if ( pre_.update_auto_orient( pool_, stop_, perf_, src, ch, fd_src, analytics_.frame_count( src ), publish_fn,
                                  acquire_fn ) )
        reset_source( src );
}



void Pipeline::idle_perf() noexcept
{
    const auto now = clk::now();
    watchdog_.ping_if_due( now );
    sample_rt_faults_if_due( perf_, now );
    perf_.tick( now, sources_.sources(), output_ );
}

void Pipeline::step_done_perf( PerfClock::time_point t0 ) noexcept
{
    const auto now = clk::now();
    watchdog_.ping_if_due( now );
    sample_rt_faults_if_due( perf_, now );
    perf_.step_done( t0, now, sources_.sources(), output_ );
}

Pipeline::Step Pipeline::step( int& ok, Status& st ) noexcept
{
    st = Status {};
    const auto t0 = clk::now();
    EncodedFrame f {};
    const auto drain_fn = [this, &ok]() noexcept { drain_done_slots( ok ); };
    const auto idle_fn = [this]() noexcept {
        idle_perf();
        output_.idle_tick( t0_, sources_.sources() );
    };
    const auto reset_fn = [this]( int s ) noexcept { reset_source( s ); };

    const FrameStatus fs = sources_.next_frame( f, pool_, drain_fn, idle_fn );
    if ( fs != FrameStatus::Frame )
    {
        if ( fs == FrameStatus::Stopped || stop_.requested() )
            return Step::Stop;
        if ( const Status hst = sources_.handle_failure( f, pool_, reset_fn ); !hst.ok() )
        {
            st = hst;
            return Step::Fatal;
        }
        step_done_perf( t0 );
        return Step::Skipped;
    }

    const auto t1 = clk::now();
    perf_.frame( f.src, t1 );
    Status dec_st;
    const int fd_src = sources_[f.src].decode( f, &dec_st );
    const auto t2 = clk::now();
    perf_.decode( f.src, ms( t1, t2 ) );
    if ( fd_src < 0 )
    {
        if ( !dec_st.ok() )
        {
            f.st = dec_st;
            if ( const Status hst = sources_.handle_failure( f, pool_, reset_fn ); !hst.ok() )
            {
                st = hst;
                return Step::Fatal;
            }
        }
        step_done_perf( t0 );
        return Step::Skipped;
    }

    sync_and_orient( f.src, fd_src, ok );
    if ( stop_.requested() )
        return Step::Stop;
    drain_done_slots( ok );
    if ( pool_.full() )
        ok += publish_oldest();
    const int slot = acquire_slot( ok, pre_.expected_band( f.src, sources_[f.src] ) );
    if ( slot < 0 )
        return stop_.requested() ? Step::Stop : Step::Skipped;
    const int64_t ts_us = f.cap_us >= 0
                              ? std::max<int64_t>( 0, f.cap_us - t0_cap_us_ )
                              : std::chrono::duration_cast<std::chrono::microseconds>( t1 - t0_ ).count();
    pre_.frame_dispatch( pool_, slot, f.src, sources_[f.src], analytics_.next_frame_index( f.src ), fd_src, ts_us,
                         ms( t0, t1 ), ms( t1, t2 ) );
    if ( pool_.size() == 1 )
        ok += publish_oldest();
    step_done_perf( t0 );
    return Step::Dispatched;
}

Status Pipeline::run()
{
    int ok = 0;
    // Hot-attach prober: local so its destructor stops and joins it on any
    // exit. Spawned BEFORE the RT pin so it keeps the scheduler default.
    SourceSupervisor::SourceProber prober( sources_.sources(), sources_.locator(), log_ );
    prober.start();
    analytics_.init_box_logs( cfg_.box_log, sources_.size() );
    prefault_current_thread_stack();
    pin_current_thread_rt( "pipeline main", 4, 80, log_ );

    t0_ = clk::now();
    t0_cap_us_ = std::chrono::duration_cast<std::chrono::microseconds>( t0_.time_since_epoch() ).count();
    perf_.start( t0_ );
    perf_.enable_rt( 60.0 );
    {
        uint64_t minflt = 0;
        uint64_t majflt = 0;
        read_thread_page_faults( minflt, majflt );
        perf_.seed_rt_faults( minflt, majflt );
    }
    sources_.reset_stall_clocks( t0_ );
    watchdog_.notify_ready();
    watchdog_.ping_now( t0_ );

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
    watchdog_.notify_stopping();
    if ( stop_.requested() )
        log_.info() << "stop: signal received, draining and flushing...\n";
    while ( !pool_.empty() )
        ok += publish_oldest();
    output_.drain();
    t_.report_total();
    output_.summary( ok, std::chrono::duration<double>( clk::now() - t0_ ).count(), sources_.reconnects() );
    return {};
}

int Pipeline::publish_oldest() noexcept
{
    if ( !pool_.collect( res_ ) )
        return 0;

    double enc_ms = 0.0;
    if ( res_.npu_ok )
    {
        FrameView view {};
        view.tensor_idx = res_.slot;
        view.tensor = pool_.tensor( res_.slot );
        view.lb = res_.job.lb;
        view.src_idx = res_.job.src_idx;
        view.src_w = res_.job.src_w;
        view.src_h = res_.job.src_h;
        view.ts_us = res_.job.ts_us;

        analytics_.process( res_, view, pre_.rga() );
        const bool defer_enc = sources_.size() > 1 && pool_.front_done();
        enc_ms = output_.publish_frame( res_.job.frame, view, analytics_.dets(), analytics_.trajs(),
                                        analytics_.tracker( res_.job.src_idx ), sources_.size(), defer_enc );
    }

    res_.job.tr.enc = enc_ms;
    t_.add( res_.job.tr, res_.npu_ok );
    return res_.npu_ok ? 1 : 0;
}
