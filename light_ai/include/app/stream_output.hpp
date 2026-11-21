// StreamOutput: output stage owning OverlaySink, ConsoleReporter, dirty-tile
// flushing, and paced NO SIGNAL idle publishing on a dedicated SPSC drop-oldest
// worker thread (Stage F).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "app/console_reporter.hpp"
#include "config/config.hpp"
#include "core/callout.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/geometry.hpp"
#include "core/global_track.hpp"
#include "core/gpu_box.hpp"
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/perf_reporter.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"
#include "platform/joining_thread.hpp"
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
    ~StreamOutput();

    StreamOutput( const StreamOutput& ) = delete;
    StreamOutput& operator=( const StreamOutput& ) = delete;

    void init( const StreamConfig& stream, const TensorSet& tensors, int model_w, int model_h,
               const LbGeom& enc_lb, int nsrc = 1 );

    void reset_source( int src ) noexcept;
    void flush_dirty() noexcept;
    void wait_slot_free( int slot ) noexcept;
    uint32_t busy_slot_mask() const noexcept;
    void drain() noexcept;

    double publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                          const Tracker& tracker, int nsrc, bool defer_enc ) noexcept;

    template <class Srcs>
    void idle_tick( PerfClock::time_point t0, const Srcs& srcs ) noexcept
    {
        flush_dirty();
        if ( !sink_.enabled() || !sink_.viewer_live() )
            return;
        const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   PerfClock::now() - t0 )
                                   .count();
        const int64_t step_us = fps_ > 0 ? 1000000LL / fps_ : 40000;
        if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
            return;
        std::array<bool, kMaxSources> absent {};
        const bool any_absent = srcs.absent_mask( absent );
        if ( !any_absent && srcs.size() > 0 )  // no configured sources: one gray canvas
            return;
        last_idle_us_ = now_us;
        post_idle( now_us, absent, srcs.size() );
    }

    OverlayPerfWindow take_perf() noexcept;
    bool has_enc_perf() const noexcept { return sink_.has_enc_perf(); }
    EncPerfWindow take_enc_perf() noexcept;

    void summary( int frames, double elapsed_sec, int reconnects ) const noexcept;

private:
    struct PublishJob
    {
        FrameView view {};
        GpuBoxList boxes {};
        CalloutList callouts {};
        bool defer_enc = false;
    };

    struct IdleJob
    {
        int64_t ts_us = 0;
        std::array<bool, kMaxSources> absent {};
        int nabs = 0;
    };

private:
    void stop_worker() noexcept;
    void post_idle( int64_t ts_us, const std::array<bool, kMaxSources>& absent, int nabs ) noexcept;
    void worker_loop() noexcept;
    void consume_controls() noexcept;
    bool any_pending_locked() const noexcept;
    bool slot_pending_locked( int slot ) const noexcept;
    bool take_work( PublishJob& pjob, bool& has_pub, IdleJob& ijob, bool& has_idle ) noexcept;

private:
    const Log& log_;
    OverlaySink sink_;
    ConsoleReporter rep_;
    int fps_ = 0;
    int64_t last_idle_us_ = -1;

    mutable std::mutex mx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool flush_req_ = false;
    std::array<bool, kMaxSources> has_pending_ {};
    std::array<PublishJob, kMaxSources> pending_ {};
    int next_src_ = 0;
    bool has_idle_ = false;
    IdleJob idle_ {};
    int active_slot_ = -1;

    std::mutex perf_mx_;
    OverlayPerfWindow perf_snap_ {};
    EncPerfWindow enc_perf_snap_ {};

    JoiningThread worker_;
};
