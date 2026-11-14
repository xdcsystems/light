// MuxSession: the network side of the H.264 preview -- the serve thread.
// It waits for a viewer (TCP pull: listen + accept on cfg.port; UDP push:
// avio_open to cfg.host:port), sets up an MPEG-TS muxer per viewer
// session and writes the packets the encoder queues, until the viewer
// goes, the encoder fails the session or stop() is called; then it waits
// for the next viewer.
//
// Shared with the encoder (pipeline thread) -- and nothing else:
//   PacketQueue& queue: the encoded packets (pop), the session frame
//     (open / close);
//   SessionGate& gate:  the session hand-over (open / close).
// Session order (#19, review of the 4a plan):
//   open:  queue.open( epoch ), then gate.open( session );
//   close: gate.close() (waits for an encode in progress), then
//          queue.close(), then av_write_trailer -- only when the session
//          ended by stop() (not after an encode failure or a failed write).
// Everything else here is serve-thread state: the transport, the muxer,
// the two working packets, ParamSetCache, the session counter.
//
// Lifetime: the ctor allocates (throws on failure), start() spawns the
// thread, stop() / the dtor end it (quit + queue.stop() + join). The
// owner declares queue and gate before the session, so both outlive it.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/encoder_config.hpp"
#include "core/param_set_cache.hpp"
#include "core/session_gate.hpp"
#include "net/av_packet.hpp"
#include "net/av_packet_queue.hpp"

class Log;
class TcpPullServer;
struct AVFormatContext;
struct AVIOContext;
struct AVPacket;
struct AVStream;

class MuxSession
{
    // One viewer session's transport (TCP pull or UDP push), RAII:
    // the destructor is the single teardown point, the early-return
    // paths of run_session() free everything by scope exit.
    struct Transport
    {
        ~Transport();
        void close();

    public:
        TcpPullServer* tcp = nullptr;
        AVIOContext* udp_pb = nullptr;
        bool tcp_pull = false;
    };

    // RAII guard for the muxer context (run_session): p->pb is nulled first
    // -- the Transport destructor owns the avio context and closes it
    // -- then the context is freed. Declared before the Transport in
    // run_session(), so the context dies AFTER the Transport.
    struct FcGuard
    {
        ~FcGuard();
        FcGuard( const FcGuard& ) = delete;
        FcGuard& operator=( const FcGuard& ) = delete;

    public:
        AVFormatContext* p;
    };

    // How a session's drain loop ended: stop(), an encode-side failure of
    // this session (queue fail), or a failed write of our own.
    enum class DrainEnd : uint8_t
    {
        Stopped,
        Failed,
        WriteFailed
    };

public:
    // log: the process diagnostics, owned by main(); outlives the session
    // (Log is thread-safe). queue / gate: owned by the encoder, declared
    // before this session.
    MuxSession( const EncoderConfig& cfg, PacketQueue& queue, SessionGate& gate, const Log& log );
    ~MuxSession();
    MuxSession( const MuxSession& ) = delete;
    MuxSession& operator=( const MuxSession& ) = delete;

    void start();  // spawns the serve thread (std::system_error on failure)
    void stop();   // ends and joins it; idempotent

private:
    void serve();
    void run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url );
    // Transport setup (accept + socket options / avio_open) and the
    // stream setup (new_stream + header + mpegts options).
    bool open_transport( const std::string& url, AVFormatContext* fc, Transport& t );
    // frame_step: muxer ticks per codec pts, handed to the encoder in the gate session.
    bool setup_stream( AVFormatContext* fc, AVStream** st, int64_t* frame_step, bool tcp_pull );
    // The wait/pop loop and one packet write with the IDR prepend.
    DrainEnd drain_queue( AVFormatContext* fc );
    int mux_write( AVFormatContext* fc, AVPacket* pkt );

private:
    const Log& log_;
    const EncoderConfig cfg_;
    PacketQueue& queue_;
    SessionGate& gate_;
    AvPacket mux_pkt_;  // the packet being written (moved out of the queue)
    AvPacket dup_pkt_;  // an IDR with the SPS/PPS prefix prepended (ParamSetCache buffer)
    // Late-joiner support: the SPS/PPS prefix of the first IDR, repeated
    // on every later IDR.
    ParamSetCache params_;
    uint32_t sessions_ = 0;            // the last session's epoch (1, 2, ...)
    std::atomic<bool> quit_ { false };  // stop() -> the accept poll and the session loop
    std::thread thread_;                // last: started by start(), after every member above
};
