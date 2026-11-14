// Hardware-encoded preview (h264_rkmpp) in MPEG-TS.
// Default transport is TCP pull: the board listens on net_port,
// the viewer connects when it wants (ffplay tcp://<board-ip>:1234).
// The legacy UDP push to net_host (--net-host) remains available:
// UDP has no "connect", the viewer joins whenever it wants,
// and a lost packet glitches briefly until the next IDR
// instead of stalling the whole flow like TCP over Wi-Fi.

#pragma once

#include <cstdint>

#include "core/encode_pacer.hpp"       // EncodePacer: which publishes are encoded
#include "core/encoder_config.hpp"     // EncoderConfig, StreamLayout
#include "core/geometry.hpp"           // LbGeom: band geometry arrives as one object
#include "core/session_gate.hpp"       // SessionGate: the session hand-over
#include "net/av_packet_queue.hpp"     // PacketQueue: encoder -> serve thread
#include "net/encoder_frame_pool.hpp"  // EncoderFramePool: the encoder input ring
#include "net/frame_composer.hpp"      // FrameComposer: the picture the encoder reads
#include "net/h264_encoder.hpp"        // H264Encoder: the codec
#include "net/mux_session.hpp"         // MuxSession: the serve thread
#include "net/network_init.hpp"        // NetworkInit: avformat_network_init RAII

class Log;

class NetEncoder
{
public:
    // log: the process diagnostics, owned by main(); outlives the encoder
    // (the serve thread logs through it too: Log is thread-safe).
    // cfg: the stream geometry, rate and endpoint (core/encoder_config.hpp).
    NetEncoder( const EncoderConfig& cfg, const Log& log );

    // Trivial: every member releases its own resources, in reverse
    // declaration order (see the member block below).
    ~NetEncoder() = default;
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
    // Declaration order IS the shutdown order, read bottom-up (destruction
    // runs in reverse):
    //   1. session_  -- the serve thread: quit, queue stop, join. Nothing
    //                   touches gate_ / queue_ from the network side after.
    //   2. gate_, then queue_ -- the queued packets are unref'd while the
    //                   codec is still alive (they reference its buffers).
    //   3. codec_    -- the EOF drain, then the codec is freed: MPP lets go
    //                   of the dma-bufs before they go.
    //   4. composer_, ring_ -- the canvases, the input ring and its frames.
    //   5. net_      -- avformat_network_deinit, after everything above.
    // Construction runs top-down: net_ first, session_ last (its thread is
    // started by the ctor body once every member exists).
    const EncoderConfig cfg_;
    NetworkInit net_;
    // The encoder's input ring: 4 NV12 dma-bufs pre-bound to DRM_PRIME
    // AVFrames (net/encoder_frame_pool.hpp).
    EncoderFramePool ring_;
    // The picture: tensor band -> ring slot / canvas cell, the NO SIGNAL
    // canvas (net/frame_composer.hpp). Logs the stream layout.
    FrameComposer composer_;
    // The h264_rkmpp codec with the per-session encoder state: pts clock,
    // forced IDR, frames sent (net/h264_encoder.hpp).
    H264Encoder codec_;
    // Pipeline-thread state of publish(): which publishes are encoded
    // (core/encode_pacer.hpp) and the dequeue time handed to the codec.
    // Draw -> RGA ordering note: sync_cpu() in OverlaySink::draw_and_publish
    // runs before publish() on the same pipeline thread -- no fence fd
    // crosses this boundary (retired, see docs/state.md).
    EncodePacer pacer_;
    int64_t ts_us_ = -1;
    // Encoder -> serve thread packet queue (net/av_packet_queue.hpp): its
    // own mutex, separate from the gate's: network-side wakeups never touch
    // the encode critical section. Overflow flush, key gate, fail( epoch ).
    PacketQueue queue_;
    // The session hand-over (core/session_gate.hpp): the serve thread opens
    // and closes it, every encode runs under it. With queue_ the only state
    // the two threads share.
    SessionGate gate_;
    // The serve thread: viewer sessions, the transport, the muxer
    // (net/mux_session.hpp). Last: it holds references to queue_ and gate_.
    MuxSession session_;
};
