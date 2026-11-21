#include "app/stream_output.hpp"
#include "sys/cpu_affinity.hpp"

StreamOutput::StreamOutput( const Log& log ) : log_( log ), sink_( log ), rep_( log ) {}

StreamOutput::~StreamOutput()
{
    stop_worker();
}

void StreamOutput::init( const StreamConfig& stream, const TensorSet& tensors, int model_w, int model_h,
                         const LbGeom& enc_lb, int nsrc )
{
    fps_ = stream.fps;
    sink_.init( stream, tensors, model_w, model_h, enc_lb, nsrc );
    if ( sink_.enabled() )
        worker_.start( &StreamOutput::worker_loop, this );
}

void StreamOutput::stop_worker() noexcept
{
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void StreamOutput::flush_dirty() noexcept
{
    if ( !sink_.enabled() )
        return;
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        flush_req_ = true;
    }
    cv_.notify_one();
}

void StreamOutput::reset_source( int src ) noexcept
{
    if ( !sink_.enabled() || src < 0 || src >= kMaxSources )
        return;
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        reset_mask_ |= ( 1u << static_cast<unsigned>( src ) );
    }
    cv_.notify_one();
}

void StreamOutput::wait_slot_free( int slot ) noexcept
{
    if ( !sink_.enabled() || slot < 0 )
        return;
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this, slot]() noexcept {
        return active_slot_ != slot && !( has_pending_ && pending_.view.tensor_idx == slot );
    } );
}

void StreamOutput::drain() noexcept
{
    if ( !sink_.enabled() )
        return;
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this]() noexcept { return !has_pending_ && active_slot_ < 0; } );
}

double StreamOutput::publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                                    const Tracker& tracker, int nsrc, bool defer_enc ) noexcept
{
    rep_.report( frame, dets, trajs, view.src_idx, nsrc );
    if ( !sink_.enabled() )
        return 0.0;

    PublishJob job {};
    job.view = view;
    job.defer_enc = defer_enc;
    sink_.prepare_overlay( view, dets, trajs, tracker, job.boxes, job.callouts );
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        pending_ = job;
        has_pending_ = true;  // drop-oldest if worker has not picked up the previous frame
    }
    cv_.notify_one();
    return 0.0;
}

void StreamOutput::post_idle( int64_t ts_us, const std::array<bool, kMaxSources>& absent, int nabs ) noexcept
{
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        idle_.ts_us = ts_us;
        idle_.absent = absent;
        idle_.nabs = nabs;
        has_idle_ = true;
    }
    cv_.notify_one();
}

void StreamOutput::consume_controls() noexcept
{
    uint32_t rmask = 0;
    bool flush = false;
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        rmask = reset_mask_;
        reset_mask_ = 0;
        flush = flush_req_;
        flush_req_ = false;
    }
    for ( int s = 0; s < kMaxSources; ++s )
    {
        if ( ( rmask & ( 1u << static_cast<unsigned>( s ) ) ) != 0 )
            sink_.reset_source( s );
    }
    if ( flush )
        sink_.flush_dirty();
}

bool StreamOutput::take_work( PublishJob& pjob, bool& has_pub, IdleJob& ijob, bool& has_idle ) noexcept
{
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this]() noexcept {
        return stop_ || has_pending_ || has_idle_ || flush_req_ || reset_mask_ != 0;
    } );
    if ( stop_ && !has_pending_ )
        return false;
    has_pub = has_pending_;
    if ( has_pending_ )
    {
        pjob = pending_;
        has_pending_ = false;
        active_slot_ = pjob.view.tensor_idx;
    }
    has_idle = !has_pub && has_idle_;
    if ( has_idle )
    {
        ijob = idle_;
        has_idle_ = false;
    }
    return true;
}

void StreamOutput::worker_loop() noexcept
{
    pin_current_thread_rt( "stream output", -1, 60, log_ );
    PublishJob pjob {};
    IdleJob ijob {};
    bool has_pub = false;
    bool has_idle = false;
    while ( take_work( pjob, has_pub, ijob, has_idle ) )
    {
        consume_controls();
        if ( has_pub )
        {
            (void)sink_.render_and_publish( pjob.view, pjob.boxes, pjob.callouts, pjob.defer_enc );
            {
                const std::lock_guard<std::mutex> plk( perf_mx_ );
                perf_snap_.merge( sink_.take_perf() );
                if ( sink_.has_enc_perf() )
                    enc_perf_snap_.merge( sink_.take_enc_perf() );
            }
            {
                const std::lock_guard<std::mutex> lk( mx_ );
                active_slot_ = -1;
            }
            cv_.notify_all();
        }
        if ( has_idle )
        {
            sink_.publish_idle( ijob.ts_us, ijob.absent.data(), ijob.nabs );
            if ( sink_.has_enc_perf() )
            {
                const std::lock_guard<std::mutex> plk( perf_mx_ );
                enc_perf_snap_.merge( sink_.take_enc_perf() );
            }
        }
    }
}

OverlayPerfWindow StreamOutput::take_perf() noexcept
{
    const std::lock_guard<std::mutex> lk( perf_mx_ );
    const OverlayPerfWindow out = perf_snap_;
    perf_snap_.reset();
    return out;
}

EncPerfWindow StreamOutput::take_enc_perf() noexcept
{
    const std::lock_guard<std::mutex> lk( perf_mx_ );
    const EncPerfWindow out = enc_perf_snap_;
    enc_perf_snap_.reset();
    return out;
}

void StreamOutput::summary( int frames, double elapsed_sec, int reconnects ) const noexcept
{
    rep_.summary( frames, elapsed_sec, reconnects );
}
