// SDK-free threaded Re-ID queue coordinating 4 tensor slots between the pipeline
// thread and the Re-ID worker thread via PodRing + atomic SlotState (TSan-tested).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
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

    // Non-blocking slot claim; returns [0..3] or -1 when all slots are in flight.
    int try_acquire_slot() noexcept
    {
        const auto it = std::find_if( states_.begin(), states_.end(), []( const std::atomic<ReidSlotState>& s ) {
            return s.load( std::memory_order_acquire ) == ReidSlotState::Free;
        } );
        return it != states_.end() ? static_cast<int>( it - states_.begin() ) : -1;
    }

    bool submit( const ReidJob& job ) noexcept
    {
        if ( job.slot < 0 || job.slot >= reid_cfg::kReidSlots )
            return false;
        states_[job.slot].store( ReidSlotState::Queued, std::memory_order_release );
        {
            const std::lock_guard<light_ai::platform::PiMutex> lk( mx_ );
            if ( stop_ || jobs_.full() )
            {
                states_[job.slot].store( ReidSlotState::Free, std::memory_order_release );
                return false;
            }
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
            states_[job.slot].store( ReidSlotState::Busy, std::memory_order_release );
            ReidResult res {};
            res.src_idx = job.src_idx;
            res.local_id = job.local_id;
            res.gen = job.gen;
            res.ts_us = job.ts_us;
            res.ok = runner_( job, res );
            // Publish Free BEFORE enqueueing result so producer sees the freed slot immediately.
            states_[job.slot].store( ReidSlotState::Free, std::memory_order_release );

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
    PodRing<ReidJob, reid_cfg::kReidQueueCap> jobs_ {};
    PodRing<ReidResult, reid_cfg::kReidQueueCap> results_ {};
    light_ai::platform::PiMutex mx_;
    light_ai::platform::PiCondVar cv_;
    bool stop_ = false;
    JoiningThread worker_;
};
