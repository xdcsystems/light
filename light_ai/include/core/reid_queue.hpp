// SDK-free threaded Re-ID queue coordinating 4 tensor slots between the pipeline
// thread and the Re-ID worker thread via PodRing + atomic SlotState (TSan-tested).

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#include "core/global_track.hpp"
#include "pod_ring.hpp"

enum class ReidSlotState : uint8_t
{
    Free,
    Queued,
    Busy
};

template <class Runner>
class BasicReidQueue
{
public:
    explicit BasicReidQueue( Runner runner ) : runner_( std::move( runner ) )
    {
        for ( auto& s : states_ )
            s.store( ReidSlotState::Free, std::memory_order_relaxed );
        worker_ = std::thread( [this]() noexcept { worker_loop(); } );
    }

    ~BasicReidQueue()
    {
        {
            const std::lock_guard<std::mutex> lk( mx_ );
            stop_ = true;
        }
        cv_.notify_all();
        if ( worker_.joinable() )
            worker_.join();
    }

    BasicReidQueue( const BasicReidQueue& ) = delete;
    BasicReidQueue& operator=( const BasicReidQueue& ) = delete;
    BasicReidQueue( BasicReidQueue&& ) = delete;
    BasicReidQueue& operator=( BasicReidQueue&& ) = delete;

    // Non-blocking slot claim; returns [0..3] or -1 when all slots are in flight.
    int try_acquire_slot() noexcept
    {
        for ( int i = 0; i < reid_cfg::kReidSlots; ++i )
        {
            if ( states_[i].load( std::memory_order_acquire ) == ReidSlotState::Free )
                return i;
        }
        return -1;
    }

    bool submit( const ReidJob& job ) noexcept
    {
        if ( job.slot < 0 || job.slot >= reid_cfg::kReidSlots )
            return false;
        states_[job.slot].store( ReidSlotState::Queued, std::memory_order_release );
        {
            const std::lock_guard<std::mutex> lk( mx_ );
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
        const std::lock_guard<std::mutex> lk( mx_ );
        if ( results_.empty() )
            return false;
        out = results_.front();
        results_.pop();
        return true;
    }

private:
    void worker_loop() noexcept
    {
        while ( true )
        {
            ReidJob job {};
            {
                std::unique_lock<std::mutex> lk( mx_ );
                cv_.wait( lk, [this]() { return stop_ || !jobs_.empty(); } );
                if ( stop_ && jobs_.empty() )
                    return;
                job = jobs_.front();
                jobs_.pop();
            }

            states_[job.slot].store( ReidSlotState::Busy, std::memory_order_release );
            ReidResult res {};
            res.src_idx = job.src_idx;
            res.local_id = job.local_id;
            res.gen = job.gen;
            res.ts_us = job.ts_us;
            res.clipped = job.clipped;
            res.occluded = job.occluded;
            res.ok = runner_( job, res );
            // Publish Free BEFORE enqueueing result so producer sees the freed slot immediately.
            states_[job.slot].store( ReidSlotState::Free, std::memory_order_release );

            const std::lock_guard<std::mutex> lk( mx_ );
            if ( results_.full() )
                results_.pop();
            results_.push( res );
        }
    }

private:
    Runner runner_;
    std::array<std::atomic<ReidSlotState>, reid_cfg::kReidSlots> states_ {};
    PodRing<ReidJob, reid_cfg::kReidQueueCap> jobs_ {};
    PodRing<ReidResult, reid_cfg::kReidQueueCap> results_ {};
    std::mutex mx_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;
};
