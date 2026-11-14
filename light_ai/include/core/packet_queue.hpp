// BasicPacketQueue: the encoded packets on their way from the encoder
// (pipeline thread) to the muxer (serve thread). A fixed ring of N
// preallocated packet slots, one mutex, one condition variable: no heap
// per packet, the payload moves slot to slot (av_packet_move_ref in
// production).
//
// Thread rule:
//   pipeline thread (producer): push(), depth(), fail( epoch );
//   serve thread (consumer):    open( epoch ), pop(), close();
//   the owner's destructor:     stop() (wakes a blocked pop for good).
// Every member below is guarded by mx_; nothing is read outside it.
//
// Session frame (the serve thread, review of #19): open( epoch ) before
// the session is published to the encoder, close() after it is
// withdrawn. open() clears leftovers, arms the key gate and makes `epoch`
// the queue's session; close() clears again.
//
// Policies:
//   overflow -- the serve thread stalled (slow viewer, Wi-Fi burst).
//     Dropping one packet would tear the reference chain (every later
//     P-frame decodes into corruption until the next IDR), so push()
//     flushes the whole queue and re-arms the key gate; the caller forces
//     an IDR on the next frame. The gap is one frame, not a GOP.
//   key gate -- after open() and after an overflow, pop() drops packets
//     until the first key frame (a late join mid-GOP must not decode
//     P-frames against a missing reference).
//   fail( epoch ) -- an encode-side error ends the session: pop() returns
//     Failed. A fail() for any other session than the open one is a no-op:
//     an RGA error from a publish that began in session N cannot end
//     session N+1 (the stale-error bug of the former error_ flag).
//
// Template parameters (static polymorphism; tests/test_packet_queue.cpp
// instantiates it over a fake packet, net/av_packet_queue.hpp over AVPacket*):
//   Pkt: a slot value, default-constructible, plain assignment;
//   Ops: an object (held by value) with
//     bool init( Pkt& );                  // slot allocation, ctor only; false = failed
//     void release( Pkt& );               // slot deallocation (after unref), dtor / failed ctor
//     void unref( Pkt& );                 // drop the payload, keep the slot
//     void move( Pkt& dst, Pkt& src );    // dst := src's payload (dst unref'd first), src empty
//     bool is_key( const Pkt& ) const;    // an IDR access unit
// SDK-free; host + TSan tested.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>

#include "pod_ring.hpp"

template <class Pkt, class Ops, int N>
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

public:
    // Allocates the N slots through ops.init(); throws std::runtime_error
    // (after releasing what was allocated) if one fails.
    explicit BasicPacketQueue( Ops ops = Ops() ) : ops_( ops )
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
    // An overflow flushes the queue first (Pushed::flushed > 0): the
    // caller forces an IDR on the next frame. A closed queue drops the packet.
    Pushed push( Pkt& src )
    {
        std::lock_guard<std::mutex> lk( mx_ );
        if ( !open_ )
        {
            ops_.unref( src );
            return Pushed { 0, drops_ };
        }
        int flushed = 0;
        if ( ring_.full() )
        {
            flushed = ring_.n;
            clear_locked();
            wait_key_ = true;
            ++drops_;
        }
        ops_.move( ring_.push_slot(), src );  // !full() guaranteed above
        cv_.notify_one();
        return Pushed { flushed, drops_ };
    }

    int depth() const
    {
        std::lock_guard<std::mutex> lk( mx_ );
        return ring_.n;
    }

    // Producer: an encode-side error in session `epoch`.
    void fail( uint32_t epoch )
    {
        std::lock_guard<std::mutex> lk( mx_ );
        if ( !open_ || epoch != epoch_ )
            return;
        failed_ = true;
        cv_.notify_all();
    }

    // Consumer: session `epoch` starts.
    void open( uint32_t epoch )
    {
        std::lock_guard<std::mutex> lk( mx_ );
        clear_locked();
        wait_key_ = true;
        failed_ = false;
        epoch_ = epoch;
        open_ = true;
    }

    // Consumer: the session ended; leftover packets are dead weight.
    void close()
    {
        std::lock_guard<std::mutex> lk( mx_ );
        open_ = false;
        clear_locked();
    }

    // Consumer: blocks for the next packet (key-gated), a failure or stop().
    Popped pop( Pkt& dst )
    {
        std::unique_lock<std::mutex> lk( mx_ );
        for ( ;; )
        {
            cv_.wait( lk, [this] { return stopped_ || failed_ || ring_.n > 0; } );
            if ( stopped_ )
                return Popped { Pop::Stopped, ring_.n };
            if ( failed_ )
                return Popped { Pop::Failed, ring_.n };
            Pkt& front = ring_.front();
            if ( wait_key_ && !ops_.is_key( front ) )
            {
                ops_.unref( front );
                ring_.pop();
                continue;
            }
            wait_key_ = false;
            ops_.move( dst, front );
            ring_.pop();
            return Popped { Pop::Packet, ring_.n };
        }
    }

    // Owner: wakes the consumer for good.
    void stop()
    {
        std::lock_guard<std::mutex> lk( mx_ );
        stopped_ = true;
        cv_.notify_all();
    }

private:
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
    mutable std::mutex mx_;
    std::condition_variable cv_;
    PodRing<Pkt, N> ring_ {};
    int drops_ = 0;
    uint32_t epoch_ = 0;
    bool open_ = false;
    bool wait_key_ = true;
    bool failed_ = false;
    bool stopped_ = false;
};
