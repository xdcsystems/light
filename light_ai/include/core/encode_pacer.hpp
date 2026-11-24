// EncodePacer: which publishes become an encoded H.264 frame.
//   single view: paced to 1/fps in CFR mode (fps > 0), or every live frame in VFR (fps <= 0)
//     while backpressure is level 0;
//   multi-view: frames of different sources finish at their own native rates;
//     a frame before the next phase-locked grid tick only updates its tile in
//     the composite canvas. The grid step is 1/fps in CFR, or in follow-source
//     mode (fps <= 0) the rolling-median period of the slowest active source
//     (clamped to [33.3 ms, 200 ms], at least 40 ms while the packet queue
//     holds kHeavyDepth packets or more);
//   transport backpressure (Stage 10): tracks PacketQueue depth (32 slots) and
//     overflow flushes across levels 0..3 with hysteresis (fast step-down on
//     congestion, 8 s clean-channel hold-down before stepping back up, and
//     admission pause at >= kPauseDepth until drained <= kHeavyDepth);
//   idle (NO SIGNAL) frames: encoded unless the queue is busy, and they
//     share the budget: a live frame right after an idle one is paced
//     against the idle stamp.
// A frame without a timestamp (ts_us < 0) is always encoded unless paused
// by critical queue backpressure.
//
// Session epochs: a new viewer session starts the budget and backpressure
// state from scratch, so the first frame it admits is always encoded.
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
        kHeavyDepth = 12,  // deeper queue backlog: throttle VFR and resume threshold after pause
        kPauseDepth = 24,  // critical backlog (32-slot queue): pause admission to prevent qflush
        kLowDepth = 2,     // clean-channel threshold for backpressure recovery
        kMaxSources = 4,   // matches StreamLayout::kMaxTiles
        kCadenceWindow = 7
    };

public:
    EncodePacer( int nsrc, int fps ) noexcept : nsrc_( nsrc ), fps_( fps ) {}

    // Record a source publish time so multi-view VFR paces to the slowest active source.
    void observe_source( int src_idx, int64_t ts_us ) noexcept;

    // Record a PacketQueue overflow flush to escalate backpressure immediately.
    void observe_flush( int64_t ts_us, uint32_t epoch ) noexcept;

    // Estimated step (us) of the slowest active source in multi-view VFR.
    int64_t slowest_period_us( int64_t now_us ) const noexcept;

    int backpressure_level() const noexcept { return bp_level_; }
    bool backpressure_paused() const noexcept { return bp_paused_; }

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
    void update_backpressure( int64_t ts_us, int queue_depth ) noexcept;
    int64_t effective_step_us( int64_t base_step_us, int queue_depth ) const noexcept;

private:
    int nsrc_;
    int fps_;
    int64_t last_ts_us_ = -1;  // phase-locked grid stamp of this session; -1 = none yet
    uint32_t epoch_ = 0;
    int bp_level_ = 0;              // 0 = normal, 1 = mild, 2 = heavy, 3 = critical
    bool bp_paused_ = false;        // true when depth >= kPauseDepth until depth <= kHeavyDepth
    int64_t bp_high_since_us_ = -1; // continuous congestion start for step-down hold
    int64_t bp_low_since_us_ = -1;  // continuous clean-channel start for recovery hold-down
    int64_t bp_last_down_us_ = -1;  // last congestion step-down timestamp
    std::array<SourceCadence, kMaxSources> cadence_ {};
};
