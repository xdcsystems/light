#pragma once

#include <chrono>
#include <cstdint>

#include "app/analytics_stage.hpp"
#include "app/preprocessor.hpp"
#include "app/source_supervisor.hpp"
#include "app/stream_output.hpp"
#include "config/config.hpp"
#include "core/log.hpp"
#include "core/perf_reporter.hpp"
#include "core/status.hpp"
#include "core/stop_flag.hpp"
#include "core/timings.hpp"
#include "npu/inference_pool.hpp"
#include "npu/rknn_engine.hpp"
#include "npu/yolo_postprocess.hpp"

class Pipeline
{
    using NpuPool = InferencePool<RknnEngine, YoloDecode>;

    // Outcome of one frame-loop step (step()).
    enum class Step : uint8_t
    {
        Dispatched,  // a frame went to the pool: counts against --frames
        Skipped,     // nothing to count: empty packet, decode error, warm-up, a handled detach
        Stop,        // stop requested: leave through the drain/summary path
        Fatal        // unrecoverable source error: the reason travels in the Status out-param; run() returns it (exit code 1)
    };

public:
    // stop: set by SignalWaiter on SIGINT/SIGTERM; log: the process diagnostics.
    // Both are owned by main() and outlive the Pipeline.
    Pipeline( const Config& cfg, const StopFlag& stop, const Log& log );
    ~Pipeline() = default;  // pool_ joins its workers before its engines die
    Pipeline( const Pipeline& ) = delete;
    Pipeline& operator=( const Pipeline& ) = delete;
    Pipeline( Pipeline&& ) = delete;
    Pipeline& operator=( Pipeline&& ) = delete;

    // Exits the frame loop through the normal drain/summary path once
    // the stop flag is set (graceful shutdown, P1-6). Status: Failed on a
    // frame-path fatal (exit code 1); a clean end is Ok even with zero
    // published frames.
    Status run();

private:
    // One frame: next_frame -> decode -> orient -> dispatch -> publish (pool 1).
    // st: the fatal reason, filled only on Step::Fatal (cleared at entry).
    Step step( int& ok, Status& st ) noexcept;
    void step_done_perf( PerfClock::time_point t0 ) noexcept;
    void idle_perf() noexcept;
    void sync_and_orient( int src, int fd_src, int& ok ) noexcept;
    void drain_done_slots( int& ok ) noexcept;
    int publish_oldest() noexcept;  // collect -> analytics -> output; 1 if valid
    int acquire_slot( int& ok ) noexcept;
    void reset_source( int src ) noexcept;
    int64_t now_us() const noexcept;

private:
    Config cfg_;
    SourceSupervisor sources_;
    // NPU stage: declared before output_ so OverlaySink's GPU drawer (which
    // imports pool_'s tensors) is destroyed before the pool's engines die.
    NpuPool pool_;
    Preprocessor pre_;
    AnalyticsStage analytics_;
    StreamOutput output_;
    Timings t_;        // avg/60 + whole-run TOTAL; logs through log_
    InferResult res_;  // preallocated per-frame result buffer
    const StopFlag& stop_;
    const Log& log_;
    std::chrono::steady_clock::time_point t0_ {};  // run() start: pts base
    int64_t t0_cap_us_ = 0;
    PerfReporter perf_;
};
