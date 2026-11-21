// AsyncLogSink: lock-free MPSC bounded ring buffer drained by a SCHED_OTHER
// background thread pinned to Cortex-A55 (cpu0-3), eliminating mutex/I/O
// priority inversion from RT threads. Drop-on-overflow with atomic counter.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <ostream>
#include <sched.h>
#include <thread>

#include "core/log.hpp"
#include "platform/joining_thread.hpp"

namespace light_ai
{
namespace platform
{
namespace detail
{
inline void pin_drainer_a55() noexcept
{
    cpu_set_t set {};
    CPU_ZERO( &set );
    for ( int c = 0; c < 4; ++c )
        CPU_SET( c, &set );
    ::sched_setaffinity( 0, sizeof( set ), &set );
}
}  // namespace detail

class AsyncLogSink
{
public:
    enum
    {
        kCapacity = 512,
        kMask = kCapacity - 1,
        kMaxLineBytes = 512
    };
    static_assert( ( kCapacity & kMask ) == 0, "AsyncLogSink::kCapacity must be power of two" );

public:
    AsyncLogSink( std::ostream& out, std::ostream& err, bool pin_to_a55 = true )
      : out_( out ), err_( err ), pin_to_a55_( pin_to_a55 )
    {
        for ( uint32_t i = 0; i < kCapacity; ++i )
            slots_[i].seq.store( i, std::memory_order_relaxed );
        drainer_ = JoiningThread( [this]() noexcept { drain_loop(); } );
    }

    ~AsyncLogSink()
    {
        stop_.store( true, std::memory_order_release );
        drainer_.join();
    }

    AsyncLogSink( const AsyncLogSink& ) = delete;
    AsyncLogSink& operator=( const AsyncLogSink& ) = delete;
    AsyncLogSink( AsyncLogSink&& ) = delete;
    AsyncLogSink& operator=( AsyncLogSink&& ) = delete;

    LogSinkFn sink_fn() noexcept;

    bool push( bool is_err, const char* data, std::size_t len ) noexcept
    {
        if ( data == nullptr || len == 0 )
            return true;
        uint32_t pos = head_.load( std::memory_order_relaxed );
        bool done = false;
        bool ok = false;
        while ( !done )
        {
            Slot& slot = slots_[pos & kMask];
            const uint32_t seq = slot.seq.load( std::memory_order_acquire );
            const int32_t diff = static_cast<int32_t>( seq - pos );
            if ( diff == 0 )
            {
                if ( head_.compare_exchange_weak( pos, pos + 1, std::memory_order_relaxed ) )
                {
                    const std::size_t n = std::min<std::size_t>( len, kMaxLineBytes );
                    slot.is_err = is_err;
                    slot.len = static_cast<uint16_t>( n );
                    std::memcpy( slot.data.data(), data, n );
                    slot.seq.store( pos + 1, std::memory_order_release );
                    ok = true;
                    done = true;
                }
                continue;
            }
            if ( diff < 0 )
            {
                dropped_.fetch_add( 1, std::memory_order_relaxed );
                total_dropped_.fetch_add( 1, std::memory_order_relaxed );
                ok = false;
                done = true;
                continue;
            }
            pos = head_.load( std::memory_order_relaxed );
        }
        return ok;
    }

    uint64_t dropped_count() const noexcept { return total_dropped_.load( std::memory_order_relaxed ); }

private:
    struct Slot
    {
        std::atomic<uint32_t> seq { 0 };
        bool is_err = false;
        uint16_t len = 0;
        std::array<char, kMaxLineBytes> data {};
    };

private:
    bool drain_one() noexcept
    {
        Slot& slot = slots_[tail_ & kMask];
        const uint32_t seq = slot.seq.load( std::memory_order_acquire );
        if ( seq != tail_ + 1 )
            return false;
        try
        {
            std::ostream& os = slot.is_err ? err_ : out_;
            os.write( slot.data.data(), static_cast<std::streamsize>( slot.len ) );
        }
        catch ( const std::exception& )
        {
        }
        slot.seq.store( tail_ + kCapacity, std::memory_order_release );
        ++tail_;
        return true;
    }

    void flush_dropped() noexcept
    {
        const uint64_t d = dropped_.exchange( 0, std::memory_order_relaxed );
        if ( d == 0 )
            return;
        try
        {
            err_ << "log: dropped " << d << " lines\n";
        }
        catch ( const std::exception& )
        {
        }
    }

    void drain_loop() noexcept
    {
        if ( pin_to_a55_ )
            detail::pin_drainer_a55();
        bool drained_any = false;
        while ( !stop_.load( std::memory_order_acquire ) )
        {
            if ( drain_one() )
            {
                drained_any = true;
                continue;
            }
            flush_dropped();
            if ( drained_any )
            {
                try
                {
                    out_.flush();
                    err_.flush();
                }
                catch ( const std::exception& )
                {
                }
                drained_any = false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        bool drained = true;
        while ( drained )
            drained = drain_one();
        flush_dropped();
        try
        {
            out_.flush();
            err_.flush();
        }
        catch ( const std::exception& )
        {
        }
    }

private:
    std::ostream& out_;
    std::ostream& err_;
    const bool pin_to_a55_;
    std::array<Slot, kCapacity> slots_ {};
    alignas( 64 ) std::atomic<uint32_t> head_ { 0 };
    alignas( 64 ) uint32_t tail_ = 0;
    std::atomic<uint64_t> dropped_ { 0 };
    std::atomic<uint64_t> total_dropped_ { 0 };
    std::atomic<bool> stop_ { false };
    JoiningThread drainer_;
};

namespace detail
{
inline void async_log_write_cb( void* ctx, bool is_err, const char* data, std::size_t len ) noexcept
{
    if ( ctx != nullptr )
        static_cast<AsyncLogSink*>( ctx )->push( is_err, data, len );
}
}  // namespace detail

inline LogSinkFn AsyncLogSink::sink_fn() noexcept { return { this, &detail::async_log_write_cb }; }

}  // namespace platform
}  // namespace light_ai
