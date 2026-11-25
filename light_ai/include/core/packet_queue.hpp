// BasicPacketQueue: the encoded packets from the encoder (pipeline thread)
// to the muxer (serve thread). A fixed ring of preallocated packet slots,
// one mutex, one condition variable, no heap per packet; the payload
// moves slot to slot (av_packet_move_ref in production).
//
// Thread rule (every member is guarded by mx_):
//   pipeline thread (producer): push(), depth(), backlog(), fail( epoch );
//   serve thread (consumer):    open( epoch ), pop(), close();
//   the owner's destructor:     stop() (wakes a blocked pop for good).
//
// Session (the serve thread): open( epoch ) before the session is published
// to the encoder, close() after it is withdrawn. open() clears leftovers,
// arms the key gate and makes `epoch` the session; close() clears again.
//
// Overflow (the serve thread stalled): push() flushes the whole queue and
// re-arms the key gate; the caller forces an IDR on the next frame -- a
// dropped packet would tear the reference chain, later P-frames decode into
// corruption until the next IDR; the gap is one frame, not a GOP.
// Key gate: after open() and after an overflow, pop() drops packets until
// the first key frame (a late join mid-GOP must not decode P-frames against
// a missing reference).
// fail( epoch ): an encode-side error ends the session, pop() returns
// Failed. A fail() for another session than the open one is a no-op: an RGA
// error from a publish begun in session N cannot end session N+1.
//
// Pkt/Ops contracts: the production instantiation is net/av_packet_queue.hpp,
// the test one tests/test_packet_queue.cpp.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>

#include "core/pod_ring.hpp"

inline int64_t packet_queue_now_us() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch() )
        .count();
}

template <class Pkt, class Ops, int N, class Mutex = std::mutex, class CondVar = std::condition_variable>
class BasicPacketQueue
{
public:
    enum class Pop : uint8_t
    {
        Packet,  // dst holds the next packet
        Failed,  // fail() for the open session: the session ends
        Stopped  // stop(): the owner is going away
    };

    struct Pushed
    {
        int flushed = 0;  // packets flushed by an overflow; 0 = none
        int drops = 0;    // overflow flushes so far (for the log line)
    };

    struct Popped
    {
        Pop what = Pop::Stopped;
        int left = 0;  // packets still queued after this one
    };

    struct Backlog
    {
        int depth = 0;            // live packets queued (excluding wait_key_ dead weight)
        int64_t head_age_us = 0;  // age of the oldest live packet; 0 when empty
    };

public:
    enum
    {
        kDefaultMaxAgeUs = 300000  // 300 ms max packet age in queue
    };

public:
    // Allocates the N slots through ops.init(); throws std::runtime_error
    // (after releasing what was allocated) if one fails.
    explicit BasicPacketQueue( Ops ops = Ops(), int64_t max_age_us = kDefaultMaxAgeUs )
      : ops_( ops ), max_age_us_( max_age_us )
    {
        for ( int i = 0; i < N; ++i )
        {
            if ( !ops_.init( ring_.d[i] ) )
            {
                for ( int k = 0; k < i; ++k )
                    ops_.release( ring_.d[k] );
                throw std::runtime_error( "packet queue: slot allocation failed" );
            }
        }
    }

    ~BasicPacketQueue()
    {
        clear_locked();  // no other thread is left at destruction
        for ( Pkt& p : ring_.d )
            ops_.release( p );
    }

    BasicPacketQueue( const BasicPacketQueue& ) = delete;
    BasicPacketQueue& operator=( const BasicPacketQueue& ) = delete;

    // Producer: moves src's payload into the queue (src is left empty).
    // An overflow or age-limit breach flushes the queue first (Pushed::flushed > 0):
    // when flushed > 0, on_flush( src, flushed ) runs under mx_ before src is
    // moved into the ring so a queued keyframe can be rebased in place.
    // A closed queue drops the packet without calling on_flush.
    template <typename OnFlush>
    Pushed push( Pkt& src, int64_t now_us, OnFlush&& on_flush )
    {
        const std::lock_guard<Mutex> lk( mx_ );
        if ( !open_ )
        {
            ops_.unref( src );
            return Pushed { 0, drops_ };
        }
        int flushed = 0;
        bool need_flush = is_flush_needed_locked( now_us );
        if ( need_flush && wait_key_ )
        {
            while ( !ring_.empty() && !ops_.is_key( ring_.front() ) )
            {
                ops_.unref( ring_.front() );
                ring_.pop();
            }
            need_flush = is_flush_needed_locked( now_us );
        }
        if ( need_flush )
        {
            flushed = ring_.n;
            clear_locked();
            wait_key_ = true;
            ++drops_;
            if ( flushed > 0 )
                on_flush( src, flushed );
        }
        push_us_[( ring_.head + ring_.n ) % N] = now_us;
        ops_.move( ring_.push_slot(), src );  // !full() guaranteed above
        if ( ring_.n > max_depth_ )
            max_depth_ = ring_.n;
        cv_.notify_one();
        return Pushed { flushed, drops_ };
    }

    Pushed push( Pkt& src, int64_t now_us = packet_queue_now_us() )
    {
        return push( src, now_us, []( Pkt& /*src*/, int /*flushed*/ ) noexcept {} );
    }

    int depth() const
    {
        const std::lock_guard<Mutex> lk( mx_ );
        return ring_.n;
    }

    // Producer: snapshot of queued packet count and oldest packet age under mx_.
    // While wait_key_ is armed, leading non-key packets will be discarded by pop()/push()
    // and do not count as live backlog (so backpressure never blocks the needed IDR).
    Backlog backlog( int64_t now_us = packet_queue_now_us() ) const
    {
        const std::lock_guard<Mutex> lk( mx_ );
        int skip = 0;
        if ( wait_key_ )
        {
            while ( skip < ring_.n && !ops_.is_key( ring_.d[static_cast<size_t>( ( ring_.head + skip ) % N )] ) )
                ++skip;
        }
        const int live = ring_.n - skip;
        if ( live <= 0 )
            return Backlog {};
        const auto idx = static_cast<size_t>( ( ring_.head + skip ) % N );
        const int64_t age = now_us - push_us_[idx];
        return Backlog { live, age > 0 ? age : 0 };
    }

    int take_max_depth() noexcept
    {
        const std::lock_guard<Mutex> lk( mx_ );
        const int m = max_depth_;
        max_depth_ = ring_.n;
        return m;
    }

    // Producer: an encode-side error in session `epoch`.
    void fail( uint32_t epoch )
    {
        const std::lock_guard<Mutex> lk( mx_ );
        if ( !open_ || epoch != epoch_ )
            return;
        failed_ = true;
        cv_.notify_all();
    }

    // Consumer: session `epoch` starts.
    void open( uint32_t epoch )
    {
        const std::lock_guard<Mutex> lk( mx_ );
        clear_locked();
        wait_key_ = true;
        failed_ = false;
        epoch_ = epoch;
        open_ = true;
    }

    // Consumer: the session ended; leftover packets are dead weight.
    void close()
    {
        const std::lock_guard<Mutex> lk( mx_ );
        open_ = false;
        clear_locked();
    }

    // Consumer: blocks for the next packet (key-gated), a failure or stop().
    Popped pop( Pkt& dst )
    {
        std::unique_lock<Mutex> lk( mx_ );
        // The predicate drains the key gate inline: after the wait, the
        // front is a key packet (or the queue is stopped/failed).
        cv_.wait( lk, [this] {
            if ( stopped_ || failed_ )
                return true;
            while ( ring_.n > 0 && wait_key_ && !ops_.is_key( ring_.front() ) )
            {
                ops_.unref( ring_.front() );
                ring_.pop();
            }
            return ring_.n > 0;
        } );
        if ( stopped_ )
            return Popped { Pop::Stopped, ring_.n };
        if ( failed_ )
            return Popped { Pop::Failed, ring_.n };
        wait_key_ = false;
        ops_.move( dst, ring_.front() );
        ring_.pop();
        return Popped { Pop::Packet, ring_.n };
    }

    // Owner: wakes the consumer for good.
    void stop()
    {
        const std::lock_guard<Mutex> lk( mx_ );
        stopped_ = true;
        cv_.notify_all();
    }

private:
    bool is_flush_needed_locked( int64_t now_us ) const noexcept
    {
        if ( ring_.full() )
            return true;
        if ( ring_.empty() || max_age_us_ <= 0 )
            return false;
        return ( now_us - push_us_[ring_.head] ) > max_age_us_;
    }

    void clear_locked()
    {
        while ( !ring_.empty() )
        {
            ops_.unref( ring_.front() );
            ring_.pop();
        }
    }

private:
    Ops ops_;
    mutable Mutex mx_;
    CondVar cv_;
    PodRing<Pkt, N> ring_ {};
    std::array<int64_t, N> push_us_ {};
    int64_t max_age_us_ = kDefaultMaxAgeUs;
    int drops_ = 0;
    int max_depth_ = 0;
    uint32_t epoch_ = 0;
    bool open_ = false;
    bool wait_key_ = true;
    bool failed_ = false;
    bool stopped_ = false;
};
