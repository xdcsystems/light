// SDK-free threaded Re-ID queue coordinating 4 tensor slots between the pipeline
// thread and the Re-ID worker thread via PodRing + atomic SlotState (TSan-tested).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

#include "core/global_track.hpp"
#include "core/pod_ring.hpp"
#include "platform/joining_thread.hpp"
#include "platform/pi_mutex.hpp"

enum class ReidSlotState : uint8_t
{
    Free,
    Reserved,
    Queued,
    Busy
};

struct ReidNoopStart
{
    void operator()() const noexcept {}
};

template <class Runner, class OnStart = ReidNoopStart>
class BasicReidQueue
{
public:
    explicit BasicReidQueue( Runner runner, OnStart on_start = OnStart {} )
      : runner_( std::move( runner ) )
      , on_start_( std::move( on_start ) )
    {
        for ( auto& s : states_ )
            s.store( ReidSlotState::Free, std::memory_order_relaxed );
        for ( auto& c : inflight_ )
            c.store( 0, std::memory_order_relaxed );
        worker_.start( [this]() noexcept { worker_loop(); } );
    }

    ~BasicReidQueue()
    {
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            stop_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    BasicReidQueue( const BasicReidQueue& ) = delete;
    BasicReidQueue& operator=( const BasicReidQueue& ) = delete;
    BasicReidQueue( BasicReidQueue&& ) = delete;
    BasicReidQueue& operator=( BasicReidQueue&& ) = delete;

    int inflight_for( int src ) const noexcept
    {
        if ( src < 0 || src >= reid_cfg::kMaxSources )
            return 0;
        return inflight_[static_cast<size_t>( src )].load( std::memory_order_acquire );
    }

    int free_slot_count() const noexcept
    {
        int n = 0;
        for ( const auto& s : states_ )
        {
            if ( s.load( std::memory_order_acquire ) == ReidSlotState::Free )
                ++n;
        }
        return n;
    }

    // Non-blocking slot claim (Free -> Reserved via CAS); returns [0..3] or -1.
    int try_acquire_slot() noexcept
    {
        for ( int i = 0; i < reid_cfg::kReidSlots; ++i )
        {
            ReidSlotState expected = ReidSlotState::Free;
            if ( states_[static_cast<size_t>( i )].compare_exchange_strong(
                     expected, ReidSlotState::Reserved, std::memory_order_acq_rel, std::memory_order_acquire ) )
                return i;
        }
        return -1;
    }

    // Releases a Reserved slot if the caller aborts before submit() (e.g. RGA crop error).
    void release_slot( int slot ) noexcept
    {
        if ( slot < 0 || slot >= reid_cfg::kReidSlots )
            return;
        ReidSlotState expected = ReidSlotState::Reserved;
        states_[static_cast<size_t>( slot )].compare_exchange_strong(
            expected, ReidSlotState::Free, std::memory_order_acq_rel, std::memory_order_relaxed );
    }

    bool submit( const ReidJob& job ) noexcept
    {
        if ( job.slot < 0 || job.slot >= reid_cfg::kReidSlots )
            return false;
        const auto idx = static_cast<size_t>( job.slot );
        ReidSlotState cur = states_[idx].load( std::memory_order_acquire );
        while ( cur == ReidSlotState::Reserved || cur == ReidSlotState::Free )
        {
            if ( states_[idx].compare_exchange_weak(
                     cur, ReidSlotState::Queued, std::memory_order_acq_rel, std::memory_order_acquire ) )
                break;
        }
        if ( cur != ReidSlotState::Reserved && cur != ReidSlotState::Free )
            return false;
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( stop_ || jobs_.full() )
            {
                states_[idx].store( ReidSlotState::Free, std::memory_order_release );
                return false;
            }
            if ( job.src_idx >= 0 && job.src_idx < reid_cfg::kMaxSources )
                inflight_[static_cast<size_t>( job.src_idx )].fetch_add( 1, std::memory_order_release );
            jobs_.push( job );
        }
        cv_.notify_one();
        return true;
    }

    bool poll_result( ReidResult& out ) noexcept
    {
        const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
        if ( results_.empty() )
            return false;
        out = results_.front();
        results_.pop();
        return true;
    }

private:
    // Blocks for the next job; false once stop is requested and the queue is drained.
    bool next_job( ReidJob& job ) noexcept
    {
        std::unique_lock<light_ai::platform::PiMutex> lk( mx_ );
        cv_.wait( lk, [this]() { return stop_ || !jobs_.empty(); } );
        if ( stop_ && jobs_.empty() )
            return false;
        job = jobs_.front();
        jobs_.pop();
        return true;
    }

    void worker_loop() noexcept
    {
        on_start_();
        ReidJob job {};
        while ( next_job( job ) )
        {
            const auto idx = static_cast<size_t>( job.slot );
            states_[idx].store( ReidSlotState::Busy, std::memory_order_release );
            ReidResult res {};
            res.src_idx = job.src_idx;
            res.local_id = job.local_id;
            res.gen = job.gen;
            res.ts_us = job.ts_us;
            res.ok = runner_( job, res );
            // Decrement inflight and publish Free BEFORE enqueueing result (immune to results_ eviction).
            if ( job.src_idx >= 0 && job.src_idx < reid_cfg::kMaxSources )
                inflight_[static_cast<size_t>( job.src_idx )].fetch_sub( 1, std::memory_order_release );
            states_[idx].store( ReidSlotState::Free, std::memory_order_release );

            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( results_.full() )
                results_.pop();
            results_.push( res );
        }
    }

private:
    Runner runner_;
    OnStart on_start_;
    std::array<std::atomic<ReidSlotState>, reid_cfg::kReidSlots> states_ {};
    std::array<std::atomic<int>, reid_cfg::kMaxSources> inflight_ {};
    PodRing<ReidJob, reid_cfg::kReidQueueCap> jobs_ {};
    PodRing<ReidResult, reid_cfg::kReidQueueCap> results_ {};
    light_ai::platform::PiMutex mx_;
    light_ai::platform::PiCondVar cv_;
    bool stop_ = false;
    JoiningThread worker_;
};
