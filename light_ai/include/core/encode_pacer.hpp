// EncodePacer: which publishes become an encoded H.264 frame.
//   single view: every live frame is encoded;
//   multi-view: frames of different sources finish within a few ms of each
//     other; a frame closer than the minimum step to the last encode only
//     updates its tile in the composite canvas. The step is 1/fps, or in
//     follow-source mode (fps <= 0) 4 ms -- 20 ms while the packet queue
//     holds kBusyDepth packets or more (the mux side lags);
//   idle (NO SIGNAL) frames: encoded unless the queue is busy, and they
//     share the budget: a live frame right after an idle one is paced
//     against the idle stamp.
// A frame without a timestamp (ts_us < 0) is always encoded.
//
// Session epochs: a new viewer session starts the budget from scratch, so
// its first live frame is always encoded. The caller passes the epoch of
// the session it is publishing into (a snapshot taken at the top of the
// publish); a new epoch drops the stamp of the previous session before
// the frame is judged.
//
// SDK-free, host-tested (tests/test_encode_pacer.cpp). Thread: the
// pipeline thread only.

#pragma once

#include <cstdint>

class EncodePacer
{
public:
    enum
    {
        kBusyDepth = 4  // queued packets at which the mux side counts as lagging
    };

public:
    EncodePacer( int nsrc, int fps ) : nsrc_( nsrc ), fps_( fps ) {}

    // Live frame: true = encode it (the stamp is taken), false = canvas only.
    bool admit( int64_t ts_us, int queue_depth, uint32_t epoch );

    // Idle frame: false = skip this tick (queue busy); true = encode it (stamped).
    bool admit_idle( int64_t ts_us, int queue_depth, uint32_t epoch );

private:
    void enter( uint32_t epoch );

private:
    int nsrc_;
    int fps_;
    int64_t last_ts_us_ = -1;  // last encoded frame of this session; -1 = none yet
    uint32_t epoch_ = 0;
};
