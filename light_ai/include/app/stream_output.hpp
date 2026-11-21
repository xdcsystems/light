// StreamOutput: output stage owning OverlaySink, ConsoleReporter, dirty-tile
// flushing, and paced NO SIGNAL idle publishing. Extracted from Pipeline.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

#include "app/console_reporter.hpp"
#include "config/config.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/global_track.hpp"
#include "core/log.hpp"
#include "core/perf_reporter.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"
#include "render/overlay_sink.hpp"

class StreamOutput
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources
    };

public:
    explicit StreamOutput( const Log& log );
    ~StreamOutput() = default;
    StreamOutput( const StreamOutput& ) = delete;
    StreamOutput& operator=( const StreamOutput& ) = delete;
    StreamOutput( StreamOutput&& ) = delete;
    StreamOutput& operator=( StreamOutput&& ) = delete;

    void init( const StreamConfig& stream, const TensorSet& tensors, int model_w, int model_h, const LbGeom& enc_lb,
               int nsrc );

    bool enabled() const noexcept { return sink_.enabled(); }
    void flush_dirty() noexcept;
    void reset_source( int src ) noexcept { sink_.reset_source( src ); }

    double publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                          const Tracker& tracker, int nsrc, bool defer_enc ) noexcept;

    // NO SIGNAL idle publish: gray plate tiles for absent sources at a fixed
    // pace; not counted in --frames or the timing stats.
    template <class Srcs>
    void idle_tick( std::chrono::steady_clock::time_point t0, Srcs& srcs, PerfReporter& perf ) noexcept
    {
        if ( !sink_.enabled() )
            return;
        sink_.flush_dirty();
        perf.tick( PerfClock::now(), srcs, sink_ );
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - t0 )
                                   .count();
        const int64_t step_us = fps_ > 0 ? 1000000LL / fps_ : 40000;
        if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
            return;
        std::array<bool, kMaxSources> absent {};
        const bool any_absent = srcs.absent_mask( absent );
        if ( !any_absent && srcs.size() > 0 )  // no configured sources: one gray canvas
            return;
        last_idle_us_ = now_us;
        sink_.publish_idle( now_us, absent.data(), srcs.size() );
    }

    void summary( int frames, double elapsed_sec, int reconnects ) const noexcept;

    OverlaySink& sink() noexcept { return sink_; }

private:
    OverlaySink sink_;
    ConsoleReporter rep_;
    int fps_ = 0;
    int64_t last_idle_us_ = -1;
};
