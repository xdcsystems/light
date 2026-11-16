// H264Encoder: the h264_rkmpp codec -- opened with the stream geometry,
// rate and GOP of EncoderConfig, CBR forced through MPP, DRM_PRIME input
// (the dma-buf frames of EncoderFramePool: no CPU pixel copies).
//
// encode() sends one frame and queues every packet the codec returns. It
// runs under SessionGate::run() and owns the per-session encoder state:
// on the first encode of a new session epoch it restarts the pts clock
// with the session's frame_step and arms the forced IDR (every session,
// TCP or UDP, starts with an IDR). A full rkmpp input queue drops the frame
// and forces an IDR next; a packet-queue overflow forces one too.
//
// The destructor drains the codec (EOF, then every pending packet) while
// its MPP context is alive, then frees it: without the flush the wrapper
// tears its pools down first and mpp_frame_deinit runs on the leftovers
// ("non-positive ref_count" / "invalid mem pool ptr" at exit). The owner
// destroys the packet queue BEFORE this object (the queued packets
// reference codec buffers) and the frame pool AFTER it (MPP references
// the dma-bufs).
// Thread: the pipeline thread only (ctor / dtor on the owner's thread).

#pragma once

#include <cstdint>

#include "core/encoder_config.hpp"
#include "core/pts_clock.hpp"
#include "core/session_gate.hpp"
#include "net/av_packet.hpp"
#include "net/av_packet_queue.hpp"
#include "core/status.hpp"

class Log;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class H264Encoder
{
    // The receive loop of drain(): its telemetry for the summary line.
    struct DrainTally
    {
        int pulled = 0;    // packets received (and dropped)
        bool eof = false;  // AVERROR_EOF reached
        int eagains = 0;   // EAGAIN answers: frames still in flight
    };

public:
    // Opens the codec; throws std::runtime_error on failure (nothing leaks).
    H264Encoder( const EncoderConfig& cfg, const Log& log );
    ~H264Encoder();
    H264Encoder( const H264Encoder& ) = delete;
    H264Encoder& operator=( const H264Encoder& ) = delete;

    // One frame of session `s` (call under SessionGate::run()). ts_us: the
    // dequeue time (VFR pts), < 0 = none. false = a codec error: the caller
    // fails the session.
    Status encode( AVFrame* fr, int64_t ts_us, const SessionGate::Session& s, PacketQueue& queue );

private:
    void enqueue( int stream_index, int64_t fpts, PacketQueue& queue );  // pkt_ -> the queue
    void request_idr();                                                  // MPP_ENC_SET_IDR_FRAME
    // EOF flush at exit (the destructor only): send_eof(), collect_pending(), the summary line.
    void drain();
    void send_eof();               // EOF into the codec; a full input queue -> pull one packet, retry
    DrainTally collect_pending();  // every pending packet until AVERROR_EOF (bounded)

private:
    const Log& log_;
    AVCodecContext* ctx_ = nullptr;  // h264_rkmpp
    AvPacket pkt_;                   // the packet being received
    PtsClock pts_;                   // restarted on a new session epoch
    bool force_idr_ = false;         // the next encode becomes an IDR
    uint32_t seen_epoch_ = 0;        // the session the state above belongs to
    // Frames encoded; gates the destructor drain in both modes (the CFR
    // counter alone would be 0 in VFR and skip the flush).
    uint64_t frames_sent_ = 0;
};
