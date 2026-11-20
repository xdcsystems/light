// PerfReporter: the source-side [perf] window of the pipeline and the owner
// of its two 5-second lines (--net active only; #110):
//   [perf] src step_max=.. | s0 f=.. dq_max=.. eagain=.. dec_max=.. or_cnt=.. or_max=.. skip=.. | s1 ...
//   [perf] enc admit=.. skip=.. idle_a=.. idle_s=.. dt_max=.. dt_hist=[..] h264_max=.. cmp_max=.. eagain=.. clamp=..
//          qflush=.. | ovl gpu_max=.. sync_max=.. ftmo=..   (no encoder: "[perf] ovl ...")
// The line format is fixed: the board and the #105 latency work read these
// lines, and test_perf_reporter pins them byte for byte.
//
// The pipeline calls the hooks; every one is a no-op while the stream is
// off (enabled == false), so nothing is collected or logged then. tick()
// reads the per-window deltas from the sources and the sink, which only need:
//   Srcs: int size(), uint32_t take_eagain_cnt( int ), operator[]( int ).skip_parts_cnt() (uint64_t, monotonic);
//   Sink: OverlayPerfWindow take_perf(), EncPerfWindow take_enc_perf(), bool has_enc_perf().
// The two formatters are plain functions in perf_reporter.cpp.
// Thread: the pipeline thread only.

#pragma once

#include <algorithm>
#include <array>

#include "core/global_track.hpp"  // reid_cfg::kMaxSources
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/perf_clock.hpp"

class PerfReporter
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources,
        kWindowMs = 5000  // one [perf] window
    };

public:
    PerfReporter( bool enabled, const Log& log ) : enabled_( enabled ), log_( log ) {}

    // run() start: the first window starts at t0.
    void start( PerfClock::time_point t0 ) noexcept { win_start_ = t0; }

    // A frame of src dequeued at tp: counted, and its gap to the previous one.
    void frame( int src, PerfClock::time_point tp ) noexcept;
    // The decode time of a frame of src.
    void decode( int src, double ms ) noexcept;
    // One auto-orient probe batch of src.
    void orient( int src, double ms ) noexcept;
    // src detached: its next frame has no dequeue gap. Ungated.
    void source_reset( int src ) noexcept;

    // The end of one frame-loop step that started at t0 (now: its end), then tick( now ).
    template <class Srcs, class Sink>
    void step_done( PerfClock::time_point t0, PerfClock::time_point now, Srcs& srcs, Sink& sink ) noexcept
    {
        if ( !enabled_ )
            return;
        step_max_.note( ms_between( t0, now ) );
        tick( now, srcs, sink );
    }

    // Logs both lines once kWindowMs have passed since the window start,
    // then starts the next window at now.
    template <class Srcs, class Sink>
    void tick( PerfClock::time_point now, Srcs& srcs, Sink& sink ) noexcept
    {
        if ( !enabled_ || ms_between( win_start_, now ) < kWindowMs )
            return;
        const int n = std::min( srcs.size(), static_cast<int>( kMaxSources ) );
        for ( int s = 0; s < n; ++s )
        {
            src_[s].eagain_cnt = srcs.take_eagain_cnt( s );
            src_[s].skip_parts_cnt = skip_[s].take( srcs[s].skip_parts_cnt() );
        }
        log_src_line( n );
        const OverlayPerfWindow op = sink.take_perf();
        const EncPerfWindow ep = sink.take_enc_perf();
        log_enc_line( sink.has_enc_perf() ? &ep : nullptr, op );
        win_start_ = now;
    }

private:
    // The src line over sources [0, n); resets their windows and the step max.
    void log_src_line( int n ) noexcept;
    // The enc + ovl line; ep == nullptr: the ovl part only (no encoder).
    void log_enc_line( const EncPerfWindow* ep, const OverlayPerfWindow& op ) const noexcept;
    bool tracked( int src ) const noexcept { return enabled_ && src >= 0 && src < kMaxSources; }

private:
    bool enabled_;
    const Log& log_;
    std::array<SourcePerfWindow, kMaxSources> src_ {};
    std::array<Lap, kMaxSources> gap_ {};             // the dequeue gap per source
    std::array<CounterDelta, kMaxSources> skip_ {};   // skip_parts_cnt per window
    MaxMs step_max_;                                  // the longest frame-loop step of the window
    PerfClock::time_point win_start_;
};
