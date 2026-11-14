// SessionWarmup: when the first frame of a viewer session may be encoded.
// Multi-view: the composite canvas is reset gray when a session starts
// (FrameComposer::reset_canvas), then every source cell has to be refreshed
// in this session -- a live band, or the NO SIGNAL plate of an absent
// source -- before the first encode, so frame 0 shows no picture of a
// previous session and no gray cell of a source that is about to publish.
// The wait is bounded: after hold_us the session is released anyway and a
// cell that got nothing stays gray. Once released, a session stays
// released; the next epoch starts over.
// Single view (one cell): released by its first publish, nothing waits.
//
// Times come from the caller (one steady-clock read per publish in
// NetEncoder), so the class is SDK-free and host-tested
// (tests/test_session_warmup.cpp). Thread: the pipeline thread only.

#pragma once

#include <cstdint>

class SessionWarmup
{
public:
    enum
    {
        kHoldUs = 200000,  // the bound on the wait: one frame of any source at 5 fps or more
        kMaxCells = 16     // the cell mask width; StreamLayout has 4 at most
    };

public:
    // ncells: the source cells to wait for (StreamLayout::nsrc), clamped to 1..kMaxCells.
    explicit SessionWarmup( int ncells, int64_t hold_us = kHoldUs );

    // First call of every publish. true = a new epoch: the cells are
    // forgotten and the hold clock starts at now_us (the caller resets the
    // canvas). The same epoch again: false, nothing changes.
    bool enter( uint32_t epoch, int64_t now_us );

    // The cell got a picture in the current epoch. Out of range: ignored.
    void refreshed( int cell );

    // true = the session may encode: every cell refreshed, or hold_us
    // elapsed since enter(). Latches for the epoch.
    bool ready( int64_t now_us );

private:
    uint32_t full_mask_;
    int64_t hold_us_;
    bool started_ = false;  // no epoch entered yet
    uint32_t epoch_ = 0;
    int64_t start_us_ = 0;
    uint32_t mask_ = 0;     // cells refreshed in this epoch
    bool released_ = false;
};
