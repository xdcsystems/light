// BasicStreamOutput: output stage owning Sink (OverlaySink in production),
// ConsoleReporter, dirty-tile flushing, and paced NO SIGNAL idle publishing on
// a dedicated SPSC drop-oldest worker thread (Stage F).
// SDK-free when instantiated with a fake Sink in host/TSan tests; OverlaySink
// is instantiated in src/app/stream_output.cpp.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "app/console_reporter.hpp"
#include "config/config.hpp"
#include "core/callout.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/global_track.hpp"
#include "core/gpu_box.hpp"
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/non_copyable.hpp"
#include "core/perf_reporter.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"
#include "platform/joining_thread.hpp"
#include "platform/pi_mutex.hpp"
#include "sys/cpu_affinity.hpp"

template <class Sink>
class BasicStreamOutput : private NonMovable<BasicStreamOutput<Sink>>
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources,
        kJobPoolSize = kMaxSources + 2  // kMaxSources pending + 1 active + 1 producer staging
    };
    static_assert( kJobPoolSize <= 31, "BasicStreamOutput::free_mask_ shift fits in uint32_t" );

public:
    explicit BasicStreamOutput( const Log& log, bool pin_worker = true )
      : log_( log ), sink_( log ), rep_( log ), pin_worker_( pin_worker )
    {
        pending_idx_.fill( -1 );
    }

    ~BasicStreamOutput()
    {
        stop_worker();
    }

    void init( const StreamConfig& stream, const TensorSet& tensors, int model_w, int model_h,
               const LbGeom& enc_lb, int nsrc = 1 )
    {
        fps_ = stream.fps;
        sink_.init( stream, tensors, model_w, model_h, enc_lb, nsrc );
        if ( sink_.enabled() )
            worker_.start( &BasicStreamOutput::worker_loop, this );
    }

    void reset_source( int src ) noexcept
    {
        sink_.reset_source( src );
    }

    void flush_dirty() noexcept
    {
        if ( !sink_.enabled() || !sink_.viewer_live() )
            return;
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            flush_req_ = true;
        }
        cv_.notify_one();
    }

    bool wait_slot_free( int slot ) noexcept
    {
        if ( !sink_.enabled() || slot < 0 )
            return true;
        std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
        const bool ok = cv_.wait_for( lk, std::chrono::milliseconds( 1500 ), [this, slot]() noexcept {
            return stop_ || ( active_slot_ != slot && !slot_pending_locked( slot ) );
        } );
        if ( ok )
            return !stop_;
        for ( int s = 0; s < kMaxSources; ++s )
        {
            const std::size_t idx = static_cast<std::size_t>( s );
            const int jidx = pending_idx_[idx];
            if ( jidx >= 0 && jobs_[static_cast<std::size_t>( jidx )].view.tensor_idx == slot )
            {
                release_job_locked( jidx );
                pending_idx_[idx] = -1;
            }
        }
        if ( active_slot_ != slot )
        {
            log_.warn() << "stream_output: evicted stalled pending job on slot " << slot << '\n';
            return true;
        }
        log_.error() << "stream_output: wait_slot_free timeout on active slot " << slot << '\n';
        return false;
    }

    uint32_t busy_slot_mask() const noexcept
    {
        if ( !sink_.enabled() )
            return 0;
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        uint32_t mask = 0;
        if ( active_slot_ >= 0 && active_slot_ < 32 )
            mask |= ( 1u << static_cast<unsigned>( active_slot_ ) );
        for ( int s = 0; s < kMaxSources; ++s )
        {
            const int jidx = pending_idx_[static_cast<std::size_t>( s )];
            if ( jidx < 0 )
                continue;
            const int tslot = jobs_[static_cast<std::size_t>( jidx )].view.tensor_idx;
            if ( tslot >= 0 && tslot < 32 )
                mask |= ( 1u << static_cast<unsigned>( tslot ) );
        }
        return mask;
    }

    void drain() noexcept
    {
        if ( !sink_.enabled() )
            return;
        std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
        const bool ok = cv_.wait_for( lk, std::chrono::milliseconds( 2000 ), [this]() noexcept {
            return stop_ || ( !any_pending_locked() && active_slot_ < 0 );
        } );
        if ( !ok )
            log_.error() << "stream_output: drain timeout\n";
    }

    double publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                          const Tracker& tracker, int nsrc, bool defer_enc ) noexcept
    {
        rep_.report( frame, dets, trajs, view.src_idx, nsrc );
        if ( !sink_.enabled() || !sink_.viewer_live() )
            return 0.0;

        PublishJob& job = jobs_[static_cast<std::size_t>( staging_idx_ )];
        job.view = view;
        job.defer_enc = defer_enc;
        job.boxes.clear();
        job.callouts.clear();
        sink_.prepare_overlay( view, dets, trajs, tracker, job.boxes, job.callouts );
        const int s = ( view.src_idx >= 0 && view.src_idx < kMaxSources ) ? view.src_idx : 0;
        const std::size_t idx = static_cast<std::size_t>( s );
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            const int prev = pending_idx_[idx];
            pending_idx_[idx] = staging_idx_;  // drop-oldest per source: swap slot index only
            staging_idx_ = ( prev >= 0 ) ? prev : acquire_free_job_locked();
        }
        cv_.notify_one();
        return 0.0;
    }

    template <class Srcs>
    void idle_tick( PerfClock::time_point t0, const Srcs& srcs ) noexcept
    {
        flush_dirty();
        if ( !sink_.enabled() || !sink_.viewer_live() )
            return;
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   PerfClock::now() - t0 )
                                   .count();
        const int64_t step_us = fps_ > 0 ? 1000000LL / fps_ : 40000;
        if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
            return;
        std::array<bool, kMaxSources> absent {};
        const bool any_absent = srcs.absent_mask( absent );
        if ( !any_absent && srcs.size() > 0 )  // no configured sources: one gray canvas
            return;
        last_idle_us_ = now_us;
        post_idle( now_us, absent, srcs.size() );
    }

    OverlayPerfWindow take_perf() noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
        const OverlayPerfWindow out = perf_snap_;
        perf_snap_.reset();
        return out;
    }

    bool has_enc_perf() const noexcept { return sink_.has_enc_perf(); }

    EncPerfWindow take_enc_perf() noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( perf_mx_ );
        const EncPerfWindow out = enc_perf_snap_;
        enc_perf_snap_.reset();
        return out;
    }

    void summary( int frames, double elapsed_sec, int reconnects ) const noexcept
    {
        rep_.summary( frames, elapsed_sec, reconnects );
    }

    Sink& sink() noexcept { return sink_; }
    const Sink& sink() const noexcept { return sink_; }

private:
    struct PublishJob
    {
        FrameView view {};
        GpuBoxList boxes {};
        CalloutList callouts {};
        bool defer_enc = false;
    };

    struct IdleJob
    {
        int64_t ts_us = 0;
        std::array<bool, kMaxSources> absent {};
        int nabs = 0;
    };

private:
    void stop_worker() noexcept
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    void post_idle( int64_t ts_us, const std::array<bool, kMaxSources>& absent, int nabs ) noexcept
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            idle_.ts_us = ts_us;
            idle_.absent = absent;
            idle_.nabs = nabs;
            has_idle_ = true;
        }
        cv_.notify_one();
    }

    void consume_controls() noexcept
    {
        bool flush = false;
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            flush = flush_req_;
            flush_req_ = false;
        }
        if ( flush )
            sink_.flush_dirty();
    }

    int acquire_free_job_locked() noexcept
    {
        for ( int i = 0; i < kJobPoolSize; ++i )
        {
            const uint32_t bit = 1u << static_cast<unsigned>( i );
            if ( ( free_mask_ & bit ) != 0 )
            {
                free_mask_ &= ~bit;
                return i;
            }
        }
        return 0;  // unreachable: kJobPoolSize == kMaxSources + 2
    }

    void release_job_locked( int jidx ) noexcept
    {
        if ( jidx >= 0 && jidx < kJobPoolSize )
            free_mask_ |= ( 1u << static_cast<unsigned>( jidx ) );
    }

    bool any_pending_locked() const noexcept
    {
        for ( int s = 0; s < kMaxSources; ++s )
        {
            if ( pending_idx_[static_cast<std::size_t>( s )] >= 0 )
                return true;
        }
        return false;
    }

    bool slot_pending_locked( int slot ) const noexcept
    {
        for ( int s = 0; s < kMaxSources; ++s )
        {
            const int jidx = pending_idx_[static_cast<std::size_t>( s )];
            if ( jidx >= 0 && jobs_[static_cast<std::size_t>( jidx )].view.tensor_idx == slot )
                return true;
        }
        return false;
    }

    bool take_work( int& pub_idx, IdleJob& ijob, bool& has_idle ) noexcept
    {
        std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
        cv_.wait( lk, [this]() noexcept {
            return stop_ || any_pending_locked() || has_idle_ || flush_req_;
        } );
        if ( stop_ && !any_pending_locked() )
            return false;
        pub_idx = -1;
        for ( int off = 0; off < kMaxSources; ++off )
        {
            const int s = ( next_src_ + off ) % kMaxSources;
            const std::size_t idx = static_cast<std::size_t>( s );
            if ( pending_idx_[idx] >= 0 )
            {
                pub_idx = pending_idx_[idx];
                pending_idx_[idx] = -1;
                next_src_ = ( s + 1 ) % kMaxSources;
                active_slot_ = jobs_[static_cast<std::size_t>( pub_idx )].view.tensor_idx;
                break;
            }
        }
        has_idle = ( pub_idx < 0 ) && has_idle_;
        if ( has_idle )
        {
            ijob = idle_;
            has_idle_ = false;
        }
        return true;
    }

    void worker_loop() noexcept
    {
        if ( pin_worker_ )
        {
            prefault_current_thread_stack();
            pin_current_thread_rt( "stream output", -1, 60, log_ );
        }
        IdleJob ijob {};
        int pub_idx = -1;
        bool has_idle = false;
        while ( take_work( pub_idx, ijob, has_idle ) )
        {
            const bool has_pub = pub_idx >= 0;
            if ( has_pub )
            {
                const PublishJob& pjob = jobs_[static_cast<std::size_t>( pub_idx )];
                static_cast<void>( sink_.render_and_publish( pjob.view, pjob.boxes, pjob.callouts, pjob.defer_enc ) );
                {
                    const std::lock_guard<light_ai::platform::PiMutex> plk( perf_mx_ );
                    perf_snap_.merge( sink_.take_perf() );
                    if ( sink_.has_enc_perf() )
                        enc_perf_snap_.merge( sink_.take_enc_perf() );
                }
                {
                    const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
                    release_job_locked( pub_idx );
                    active_slot_ = -1;
                }
                cv_.notify_all();
            }
            if ( has_idle )
                sink_.publish_idle( ijob.ts_us, ijob.absent.data(), ijob.nabs );
            bool pending_now = false;
            {
                const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
                pending_now = any_pending_locked();
            }
            if ( !pending_now )
                consume_controls();
            if ( !has_pub && sink_.has_enc_perf() )
            {
                const std::lock_guard<light_ai::platform::PiMutex> plk( perf_mx_ );
                enc_perf_snap_.merge( sink_.take_enc_perf() );
            }
        }
        sink_.release_worker_context();
    }

private:
    const Log& log_;
    Sink sink_;
    ConsoleReporter rep_;
    const bool pin_worker_;
    int fps_ = 0;
    int64_t last_idle_us_ = -1;

    mutable light_ai::platform::PiMutex mx_;
    light_ai::platform::PiCondVar cv_;
    bool stop_ = false;
    bool flush_req_ = false;
    std::array<PublishJob, kJobPoolSize> jobs_ {};
    std::array<int, kMaxSources> pending_idx_ {};
    uint32_t free_mask_ = ( ( 1u << static_cast<unsigned>( kJobPoolSize ) ) - 1u ) & ~1u;
    int staging_idx_ = 0;  // producer-only staging slot (never read by worker)
    int next_src_ = 0;
    bool has_idle_ = false;
    IdleJob idle_ {};
    int active_slot_ = -1;

    light_ai::platform::PiMutex perf_mx_;
    OverlayPerfWindow perf_snap_ {};
    EncPerfWindow enc_perf_snap_ {};

    JoiningThread worker_;
};

class OverlaySink;
using StreamOutput = BasicStreamOutput<OverlaySink>;
