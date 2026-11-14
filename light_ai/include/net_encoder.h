// Hardware-encoded preview (h264_rkmpp) in MPEG-TS.
// Default transport is TCP pull: the board listens on net_port,
// the viewer connects when it wants (ffplay tcp://<board-ip>:1234).
// The legacy UDP push to net_host (--net-host) remains available:
// UDP has no "connect", the viewer joins whenever it wants,
// and a lost packet glitches briefly until the next IDR
// instead of stalling the whole flow like TCP over Wi-Fi.

#pragma once

#include <cstdint>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>

#include "core/encode_pacer.hpp"     // EncodePacer: which publishes are encoded
#include "core/encoder_config.hpp"  // EncoderConfig, StreamLayout
#include "core/geometry.hpp"        // LbGeom: band geometry arrives as one object
#include "core/pts_clock.hpp"        // PtsClock: CFR / VFR pts
#include "core/session_gate.hpp"     // SessionGate: the session hand-over
#include "net/av_packet_queue.hpp"     // PacketQueue: encoder -> serve thread
#include "net/encoder_frame_pool.hpp"  // EncoderFramePool: the encoder input ring
#include "net/frame_composer.hpp"     // FrameComposer: the picture the encoder reads
#include "net/mux_session.hpp"        // MuxSession: the serve thread

class Log;
struct AVCodecContext;
struct AVPacket;

class NetEncoder
{
public:
    // log: the process diagnostics, owned by main(); outlives the encoder
    // (the serve thread logs through it too: Log is thread-safe).
    // cfg: the stream geometry, rate and endpoint (core/encoder_config.hpp).
    NetEncoder( const EncoderConfig& cfg, const Log& log );

    ~NetEncoder();
    NetEncoder( const NetEncoder& ) = delete;
    NetEncoder& operator=( const NetEncoder& ) = delete;

    // Called once per frame from the pipeline loop. tensor_fd is the
    // dma-buf of the NPU input tensor (RGB888, w x h); boxes are drawn.
    // ts_us is the dequeue timestamp in microseconds from pipeline
    // start -- the pts source in follow-source mode (--net-fps 0, VFR);
    // ignored in the fixed-fps mode.
    void publish( int tensor_fd, const LbGeom& band, int64_t ts_us = -1, int src_idx = 0 );

    // NO SIGNAL publish (idle): RGA-copies the pre-rendered idle canvas
    // (gray + the plate) into the ring slot / composite canvas cells of
    // every absent source, then encodes one frame. Called by
    // Pipeline::idle_tick while at least one configured tile has no live
    // source; idle encodes share the pacer budget with live
    // ones and are NOT counted in --frames or the timing stats.
    void publish_idle( int64_t ts_us, const bool* absent, int nabs );

    // True while the stream path is live (viewer connected in PULL
    // mode / UDP path open). The pipeline gates GPU box drawing on it:
    // recognition runs continuously, output does not.
    bool viewer_active() const;

private:
    // One encode under gate_.run(): starts the session's encoder state on a
    // new epoch (pts clock, forced IDR), sends the frame, queues the packets.
    bool send_frame( const SessionGate::Session& s );
    // One finished encoder packet -> the queue for the serve thread;
    // shared by the EAGAIN retry and the send drain.
    void enqueue_pkt( AVPacket* pkt, int stream_index, int64_t fpts );

private:
    const Log& log_;
    const EncoderConfig cfg_;

    // The next encode becomes an IDR (the rkmpp wrapper ignores
    // frame->key_frame, so we go via MPP). Pipeline thread only: set by a
    // TCP session's start (Session::force_idr, seen on its new epoch), a
    // dropped frame and a queue overflow.
    bool force_idr_ = false;
    uint32_t seen_epoch_ = 0;  // the session the encoder state belongs to (pipeline thread)

    AVCodecContext* enc_ = nullptr;  // h264_rkmpp
    AVPacket* pkt_ = nullptr;
    // The encoder's input ring: 4 NV12 dma-bufs pre-bound to DRM_PRIME
    // AVFrames (net/encoder_frame_pool.hpp). Freed after the codec (dtor body).
    EncoderFramePool ring_;
    // The picture: tensor band -> ring slot / canvas cell, the NO SIGNAL
    // canvas (net/frame_composer.hpp).
    FrameComposer composer_;

    // Encoder -> serve() packet queue (net/av_packet_queue.hpp): its own
    // mutex, separate from the gate's: network-side wakeups never touch the
    // encode critical section. Overflow flush, key gate, per-session fail( epoch ).
    PacketQueue queue_;
    // The session hand-over (core/session_gate.hpp): the serve thread opens
    // and closes it, every encode runs under it. With queue_ the only state
    // the two threads share.
    SessionGate gate_;
    // Draw -> RGA ordering note: sync_cpu() in OverlaySink::draw_and_publish
    // runs before publish() on the same pipeline thread -- no fence fd
    // crosses this boundary (retired, see docs/state.md).
    EncodePacer pacer_;  // which publishes are encoded (pipeline thread only)
    // CFR counter / VFR dequeue-time pts and the muxer step (core/pts_clock.hpp).
    // Pipeline thread only: restarted by send_frame() on a new session epoch
    // with the session's frame_step (pts restart from zero per viewer).
    PtsClock pts_;
    int64_t ts_us_ = -1;  // dequeue time of the frame being encoded (publish -> send_frame)
    // Frames encoded; gates the destructor drain in both modes (the CFR
    // counter alone would be 0 in VFR and skip the flush).
    uint64_t frames_sent_ = 0;

    // The serve thread: viewer sessions, the transport, the muxer
    // (net/mux_session.hpp). Last: it holds references to queue_ and gate_,
    // and the dtor body stops it before the codec goes.
    MuxSession session_;
};
