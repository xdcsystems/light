// PtsClock: the presentation timestamps of the encoded stream.
//   CFR (--net-fps N): a per-session frame counter in the codec time base
//     (1/N); every encoded or dropped frame advances it.
//   VFR (--net-fps 0): the dequeue timestamp (us) rescaled to 1/90000
//     against the session base (the first timestamped frame), clamped to
//     stay strictly increasing; without a timestamp, a 25 fps grid over the
//     frames encoded so far.
// packet_ts() converts a codec pts to the muxer time base (frame_step
// ticks per frame, set per session from the stream time base).
//
// SDK-free, host-tested (tests/test_pts_clock.cpp). The rescale is
// av_rescale( us, 90000, 1000000 ) re-implemented exactly (round to
// nearest, halves away from zero).

#pragma once

#include <cstdint>

class PtsClock
{
public:
    explicit PtsClock( bool vfr ) : vfr_( vfr ) {}

    // New viewer session: the CFR counter and the VFR base restart from zero.
    // frame_step < 1 is taken as 1.
    void restart( int64_t frame_step );

    // Codec pts of the frame about to be encoded. ts_us < 0 = no timestamp.
    // frames_sent: frames encoded so far (the VFR fallback grid).
    int64_t next( int64_t ts_us, uint64_t frames_sent );

    // Codec pts -> the muxer time base.
    int64_t packet_ts( int64_t pts ) const { return pts * frame_step_; }

    // One frame consumed a CFR slot (encoded, or dropped on a full input queue).
    void advance() { ++counter_; }

private:
    bool vfr_;
    uint64_t counter_ = 0;    // CFR pts
    int64_t base_us_ = -1;    // VFR: session base
    int64_t last_pts_ = -1;   // VFR: last pts handed out
    int64_t frame_step_ = 1;  // muxer ticks per codec pts
};

// us -> 1/90000 ticks, rounded like av_rescale (AV_ROUND_NEAR_INF).
int64_t us_to_90k( int64_t us );
