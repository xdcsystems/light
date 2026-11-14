// SessionGate: the hand-over of a viewer session from the serve thread to
// the encoder (pipeline thread). Besides PacketQueue it is the only state
// the two threads share.
//
// Thread rule:
//   serve thread:    open( session ) once the stream is set up,
//                    close() when the session ends;
//   pipeline thread: active() / epoch() lock-free (the publish fast path and
//                    viewer_active()), run( f ) for one encode: f( session )
//                    runs under the gate mutex, only while a session is open.
// close() takes the same mutex, so it waits for an encode in progress:
// after close() returns, nothing of that session is encoded or pushed.
// Every field is guarded by mx_; active_ and epoch_ are also atomics for the
// lock-free readers. open() stores epoch_ before active_, so a reader that
// sees a session active sees that session's epoch (or a later one).
//
// The encoder keeps no session state the serve thread writes. It detects a
// new session by its epoch: inside run() it restarts the pts clock and arms
// the forced IDR -- every session, TCP or UDP, starts with an IDR
// (H264Encoder::encode); the pacer budget is reset earlier,
// from the epoch snapshot publish() takes after active() (EncodePacer::admit).
//
// SDK-free; host + TSan tested (tests/test_session_gate.cpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

class SessionGate
{
public:
    struct Session
    {
        int stream_index = 0;     // the muxer's stream index for the packets
        int64_t frame_step = 1;   // muxer ticks per codec pts (PtsClock::restart)
        uint32_t epoch = 0;       // 1, 2, ... per session (PacketQueue::open / fail)
    };

public:
    // Serve thread.
    void open( const Session& s )
    {
        std::lock_guard<std::mutex> lk( mx_ );
        session_ = s;
        epoch_.store( s.epoch );
        active_.store( true );
    }

    void close()
    {
        std::lock_guard<std::mutex> lk( mx_ );
        active_.store( false );
    }

    // Pipeline thread, lock-free.
    bool active() const { return active_.load(); }
    uint32_t epoch() const { return epoch_.load(); }

    // Pipeline thread: f( const Session& ) under the gate mutex while a
    // session is open. false = no session (the frame is dropped with it).
    template <class F>
    bool run( F&& f )
    {
        std::lock_guard<std::mutex> lk( mx_ );
        if ( !active_.load() )
            return false;
        f( static_cast<const Session&>( session_ ) );
        return true;
    }

private:
    std::mutex mx_;
    Session session_ {};
    std::atomic<uint32_t> epoch_ { 0 };
    std::atomic<bool> active_ { false };
};
