// PtsClock: the presentation timestamps of the encoded stream.
//   CFR (--net-fps N): a per-session frame counter in the codec time base
//     (1/N); every encoded or dropped frame advances it.
//   VFR (--net-fps 0): the publish time (steady clock, us, read once per
//     NetEncoder::publish* / flush_dirty) rescaled to 1/90000 against the
//     session base (the first timestamped frame), clamped to stay strictly
//     increasing; without a timestamp, a 25 fps grid over the frames
//     encoded so far.
// packet_ts() converts a codec pts to the muxer time base (frame_step
// ticks per frame, set per session from the stream time base).
//
// The rescale is
// av_rescale( us, 90000, 1000000 ) re-implemented exactly (round to
// nearest, halves away from zero).

#pragma once

#include <cstdint>

#include "core/pod_ring.hpp"

class PtsClock
{
public:
    enum
    {
        kDefaultStep90k = 3600,  // 40 ms (25 fps) nominal VFR step in 1/90000 ticks
        kMinStep90k = 1800,      // 20 ms (50 fps) lower bound for tracked nominal step
        kMaxStep90k = 5400       // 60 ms (~16.7 fps) upper bound for tracked nominal step
    };

public:
    explicit PtsClock( bool vfr ) : vfr_( vfr ) {}

    // New viewer session: the CFR counter and the VFR base restart from zero.
    // frame_step < 1 is taken as 1.
    void restart( int64_t frame_step ) noexcept;

    // Codec pts of the frame about to be encoded. ts_us: its publish time
    // (VFR), < 0 = no timestamp.
    // frames_sent: frames encoded so far (the VFR fallback grid).
    int64_t next( int64_t ts_us, uint64_t frames_sent ) noexcept;

    // PacketQueue overflow flush: rebases VFR base_us_ on the next frame so
    // the post-flush PTS step does not jump by the flushed/stalled duration.
    void on_queue_flush() noexcept;

    // Re-arms the post-flush rebase across discarded non-key frames while
    // keeping the original pre-flush anchor PTS.
    void rearm_flush() noexcept;

    // Clears post-flush rebase state once the recovery IDR has landed.
    void disarm_flush() noexcept;

    // Codec pts -> the muxer time base.
    int64_t packet_ts( int64_t pts ) const noexcept { return pts * frame_step_; }

    // One frame consumed a CFR slot (encoded, or dropped on a full input queue).
    void advance() noexcept { ++counter_; }

    uint64_t clamp_cnt() const noexcept { return clamp_cnt_; }

private:
    bool vfr_;
    uint64_t counter_ = 0;                     // CFR pts
    int64_t base_us_ = -1;                     // VFR: session base
    int64_t last_pts_ = -1;                    // VFR: last pts handed out
    int64_t last_step_90k_ = kDefaultStep90k;  // VFR: recent nominal frame step
    int64_t flush_anchor_pts_ = -1;            // VFR: last_pts_ before queue flush
    bool flush_pending_ = false;               // VFR: rebase base_us_ on next timestamped frame
    int64_t frame_step_ = 1;                   // muxer ticks per codec pts
    uint64_t clamp_cnt_ = 0;                   // VFR non-monotonic clamp counter
};

// Session-aware codec->muxer PTS filter for persistent encoders: shifts each
// viewer session's codec PTS above all earlier submissions so leftover async
// packets from prior sessions are rejected deterministically before the forced
// IDR arrives, and guarantees strictly monotonic muxer PTS/DTS.
class SessionPtsFilter
{
public:
    explicit SessionPtsFilter( bool vfr ) noexcept : pts_( vfr ), vfr_( vfr ) {}

    // Starts a new viewer session: rebases codec PTS past prior submissions
    // and arms awaiting_key_ until the new session's forced IDR arrives.
    void on_epoch( int64_t frame_step ) noexcept;

    // PacketQueue overflow flush: rebases muxer-space VFR PTS continuity back to
    // the last packet not flushed from the queue and, unless the flushing packet
    // was already a keyframe, re-arms awaiting_key_.
    void on_queue_flush( bool key_queued = false, int flushed_count = 0 ) noexcept;

    // Re-anchors and rebases a keyframe that triggered a PacketQueue flush before
    // it is moved into the queue, preventing VFR wire PTS jumps.
    int64_t rebase_flushed_key( bool has_pts, int64_t raw_pkt_pts, int64_t session_fpts,
                                int flushed_count ) noexcept;

    // Computes session-relative fpts and monotonic AVFrame::pts for encode().
    int64_t next_frame_pts( int64_t ts_us, uint64_t frames_sent, int64_t& session_fpts,
                            bool is_key_frame = false ) noexcept;

    // Kept for encoder EAGAIN/drop symmetry; encode-side PTS stays un-rebased.
    void on_submit_failed( bool was_key_frame ) noexcept;

    // Forwards CFR counter advance when a frame is submitted or dropped on EAGAIN.
    void advance() noexcept { pts_.advance(); }

    // Filters/stamps one packet from avcodec_receive_packet(). Returns false when
    // the packet belongs to an earlier session or precedes the first session IDR
    // (caller must av_packet_unref and discard).
    bool stamp_packet( bool has_pts, int64_t raw_pkt_pts, bool is_key, int64_t session_fpts,
                       int64_t& mux_pts_out ) noexcept;

    uint64_t clamp_cnt() const noexcept { return pts_.clamp_cnt(); }

private:
    struct StampedPacket
    {
        int64_t rel_pts = 0;
        int64_t adj_rel_pts = 0;
        int64_t mux_pts = 0;
        int64_t step_90k = PtsClock::kDefaultStep90k;
        bool wait_key_discard = false;
    };

    enum
    {
        kQueueHistoryCap = 33  // PacketQueue capacity (32) + 1 triggering packet before flush
    };

    void commit_unconfirmed_head() noexcept;
    void set_flush_anchor( bool use_queued_key ) noexcept;
    int64_t compute_rel_pts( bool has_pts, int64_t raw_pkt_pts, int64_t session_fpts ) const noexcept;
    int64_t compute_vfr_adj( int64_t rel_pts ) noexcept;

private:
    PtsClock pts_;
    bool vfr_ = false;
    int64_t frame_step_ = 1;
    int64_t epoch_base_pts_ = 0;
    int64_t last_submitted_pts_ = -1;
    int64_t last_pkt_pts_ = -1;
    bool awaiting_key_ = true;
    bool has_stamped_ = false;

    int64_t mux_rebase_offset_ = 0;
    int64_t last_raw_rel_pts_ = -1;
    int64_t last_step_90k_ = PtsClock::kDefaultStep90k;
    bool mux_flush_pending_ = false;
    int64_t flush_anchor_rel_pts_ = -1;
    int64_t flush_anchor_adj_rel_pts_ = 0;
    int64_t flush_anchor_mux_pts_ = -1;
    int64_t flush_anchor_step_90k_ = PtsClock::kDefaultStep90k;

    PodRing<StampedPacket, kQueueHistoryCap> unconfirmed_ {};
    bool has_committed_ = false;
    StampedPacket committed_ {};
};

// us -> 1/90000 ticks, rounded like av_rescale (AV_ROUND_NEAR_INF).
int64_t us_to_90k( int64_t us ) noexcept;
