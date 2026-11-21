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

bool StreamOutput::any_pending_locked() const noexcept
{
    for ( int s = 0; s < kMaxSources; ++s )
    {
        if ( has_pending_[static_cast<size_t>( s )] )
            return true;
    }
    return false;
}

bool StreamOutput::slot_pending_locked( int slot ) const noexcept
{
    for ( int s = 0; s < kMaxSources; ++s )
    {
        const size_t idx = static_cast<size_t>( s );
        if ( has_pending_[idx] && pending_[idx].view.tensor_idx == slot )
            return true;
    }
    return false;
}

void StreamOutput::flush_dirty() noexcept
{
    if ( !sink_.enabled() || !sink_.viewer_live() )
        return;
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        flush_req_ = true;
    }
    cv_.notify_one();
}

void StreamOutput::reset_source( int src ) noexcept
{
    sink_.reset_source( src );
}

void StreamOutput::wait_slot_free( int slot ) noexcept
{
    if ( !sink_.enabled() || slot < 0 )
        return;
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this, slot]() noexcept {
        return active_slot_ != slot && !slot_pending_locked( slot );
    } );
}

uint32_t StreamOutput::busy_slot_mask() const noexcept
{
    if ( !sink_.enabled() )
        return 0;
    const std::lock_guard<std::mutex> lk( mx_ );
    uint32_t mask = 0;
    if ( active_slot_ >= 0 && active_slot_ < 32 )
        mask |= ( 1u << static_cast<unsigned>( active_slot_ ) );
    for ( int s = 0; s < kMaxSources; ++s )
    {
        const size_t idx = static_cast<size_t>( s );
        if ( has_pending_[idx] && pending_[idx].view.tensor_idx >= 0 && pending_[idx].view.tensor_idx < 32 )
            mask |= ( 1u << static_cast<unsigned>( pending_[idx].view.tensor_idx ) );
    }
    return mask;
}

void StreamOutput::drain() noexcept
{
    if ( !sink_.enabled() )
        return;
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this]() noexcept { return !any_pending_locked() && active_slot_ < 0; } );
}

double StreamOutput::publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                                    const Tracker& tracker, int nsrc, bool defer_enc ) noexcept
{
    rep_.report( frame, dets, trajs, view.src_idx, nsrc );
    if ( !sink_.enabled() || !sink_.viewer_live() )
        return 0.0;

    PublishJob job {};
    job.view = view;
    job.defer_enc = defer_enc;
    sink_.prepare_overlay( view, dets, trajs, tracker, job.boxes, job.callouts );
    const int s = ( view.src_idx >= 0 && view.src_idx < kMaxSources ) ? view.src_idx : 0;
    const size_t idx = static_cast<size_t>( s );
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        pending_[idx] = job;
        has_pending_[idx] = true;  // drop-oldest per source
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
    bool flush = false;
    {
        const std::lock_guard<std::mutex> lk( mx_ );
        flush = flush_req_;
        flush_req_ = false;
    }
    if ( flush )
        sink_.flush_dirty();
}

bool StreamOutput::take_work( PublishJob& pjob, bool& has_pub, IdleJob& ijob, bool& has_idle ) noexcept
{
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this]() noexcept {
        return stop_ || any_pending_locked() || has_idle_ || flush_req_;
    } );
    if ( stop_ && !any_pending_locked() )
        return false;
    has_pub = false;
    for ( int off = 0; off < kMaxSources; ++off )
    {
        const int s = ( next_src_ + off ) % kMaxSources;
        const size_t idx = static_cast<size_t>( s );
        if ( has_pending_[idx] )
        {
            pjob = pending_[idx];
            has_pending_[idx] = false;
            next_src_ = ( s + 1 ) % kMaxSources;
            active_slot_ = pjob.view.tensor_idx;
            has_pub = true;
            break;
        }
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
    prefault_current_thread_stack();
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
            static_cast<void>( sink_.render_and_publish( pjob.view, pjob.boxes, pjob.callouts, pjob.defer_enc ) );
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
            sink_.publish_idle( ijob.ts_us, ijob.absent.data(), ijob.nabs );
        if ( !has_pub && sink_.has_enc_perf() )
        {
            const std::lock_guard<std::mutex> plk( perf_mx_ );
            enc_perf_snap_.merge( sink_.take_enc_perf() );
        }
    }
    sink_.release_worker_context();
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
