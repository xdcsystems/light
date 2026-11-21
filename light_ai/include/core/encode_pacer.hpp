// EncodePacer: which publishes become an encoded H.264 frame.
//   single view: every live frame is encoded;
//   multi-view: frames of different sources finish at their own native rates;
//     a frame before the next phase-locked grid tick only updates its tile in
//     the composite canvas. The grid step is 1/fps in CFR, or in follow-source
//     mode (fps <= 0) the rolling-median period of the slowest active source
//     (clamped to [33.3 ms, 200 ms], at least 40 ms while the packet queue
//     holds kHeavyDepth packets or more);
//   idle (NO SIGNAL) frames: encoded unless the queue is busy, and they
//     share the budget: a live frame right after an idle one is paced
//     against the idle stamp.
// A frame without a timestamp (ts_us < 0) is always encoded.
//
// Session epochs: a new viewer session starts the budget from scratch, so
// the first frame it admits is always encoded. The caller passes the epoch
// of the session it is publishing into (a snapshot taken at the top of the
// publish); a new epoch drops the stamp of the previous session before
// the frame is judged. NetEncoder consults the pacer only once the session
// warmup has released the session (core/session_warmup.hpp): held
// publishes never stamp it.
//
// Thread:
// pipeline thread only.

#pragma once

#include <array>
#include <cstdint>

class EncodePacer
{
public:
    enum
    {
        kBusyDepth = 4,    // queued packets at which the mux side counts as lagging
        kHeavyDepth = 12,  // deeper queue backlog: throttle VFR to at least 40 ms (25 fps)
        kMaxSources = 4,   // matches StreamLayout::kMaxTiles
        kCadenceWindow = 7
    };

public:
    EncodePacer( int nsrc, int fps ) noexcept : nsrc_( nsrc ), fps_( fps ) {}

    // Record a source publish time so multi-view VFR paces to the slowest active source.
    void observe_source( int src_idx, int64_t ts_us ) noexcept;

    // Estimated step (us) of the slowest active source in multi-view VFR.
    int64_t slowest_period_us( int64_t now_us ) const noexcept;

    // Live frame: true = encode it (the stamp is taken), false = canvas only.
    bool admit( int64_t ts_us, int queue_depth, uint32_t epoch ) noexcept;

    // Idle frame: false = skip this tick (queue busy); true = encode it (stamped).
    bool admit_idle( int64_t ts_us, int queue_depth, uint32_t epoch ) noexcept;

private:
    struct SourceCadence
    {
        int64_t last_us = -1;
        std::array<int64_t, kCadenceWindow> dts {};
        int count = 0;
        int cursor = 0;
    };

    void enter( uint32_t epoch ) noexcept;
    void advance_grid( int64_t ts_us, int64_t step_us ) noexcept;

private:
    int nsrc_;
    int fps_;
    int64_t last_ts_us_ = -1;  // phase-locked grid stamp of this session; -1 = none yet
    uint32_t epoch_ = 0;
    std::array<SourceCadence, kMaxSources> cadence_ {};
};
