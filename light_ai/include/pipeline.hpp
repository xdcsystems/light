#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "box_smoother.h"
#include "cal_dumper.hpp"
#include "core/status.hpp"
#include "config/config.hpp"
#include "console_reporter.h"
#include "core/auto_orient.hpp"
#include "core/log.hpp"
#include "core/net_perf.hpp"
#include "core/stop_flag.hpp"
#include "frame_trace.h"
#include "core/global_track.hpp"
#include "inference_pool.hpp"
#include "overlay_sink.hpp"
#include "reid_worker.hpp"
#include "rga_resizer.h"
#include "rknn_engine.hpp"
#include "source/source_prober.hpp"
#include "source/source_set.hpp"
#include "source/source_types.hpp"
#include "sources.hpp"
#include "timings.hpp"
#include "tracker.hpp"
#include "yolo_postprocess.hpp"

class Pipeline
{
    enum { kMaxSources = reid_cfg::kMaxSources };
    using NpuPool = InferencePool<RknnEngine, YoloDecode>;
    using SourceSet = BasicSourceSet<SourceChannel, kMaxSources>;
    using SourceProber = BasicSourceProber<SourceSet, UsbNodeLocator>;

    // Outcome of one frame-loop step (step()).
    enum class Step : uint8_t
    {
        Dispatched,  // a frame went to the pool: counts against --frames
        Skipped,     // nothing to count: empty packet, decode error, warm-up, a handled detach
        Stop,        // stop requested: leave through the drain/summary path
        Fatal        // unrecoverable source error: the reason travels in the Status out-param; run() returns it (exit code 1)
    };

    // Per-source analytics, frame thread only (publish_oldest): the
    // source itself -- device, decoder, geometry, state -- is SourceChannel.
    struct SourceAnalytics
    {
        BoxSmoother smoother {};
        Tracker tracker {};
        AutoOrient orient {};
        uint32_t seen_attach_seq = 0;
        int cur_rot = 0;
        int frames = 0;  // frames dispatched from this source (the cal/box-log frame index)
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
    void attach_sources();  // ctor, last: the first attach of every channel (STREAMON)

    // One frame: next_frame -> decode -> dispatch -> publish (pool 1).
    // st: the fatal reason, filled only on Step::Fatal (cleared at entry).
    Step step( int& ok, Status& st ) noexcept;
    // The next encoded frame or a failure; parks (NO SIGNAL, 40 ms quanta)
    // while no source is Live. Stopped once the stop flag is set.
    FrameStatus next_frame( EncodedFrame& f, int& ok );
    void drain_done_slots( int& ok );
    int publish_oldest();                // collect -> analytics -> output; 1 if the frame was valid
    void run_reid_stage( const FrameView& view ) noexcept;

    // NO SIGNAL idle publish: gray plate tiles for absent sources at a
    // fixed pace; not counted in --frames or the timing stats.
    void idle_tick();
    void reset_source_analytics( int src ) noexcept;
    Status handle_source_failure( const EncodedFrame& f );  // the fatal reason; Ok = recovered
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop

    void prepare_tensor_band( int slot, const LbGeom& eff_lb ) noexcept;
    bool submit_orient_probe( int src, int fd_src, int rot, int job_idx, int& ok ) noexcept;
    bool probe_rot_batch( int src, int fd_src, const int* rots, int n_rots, float* scores, int& ok ) noexcept;
    void update_auto_orient( int src, int fd_src, int& ok ) noexcept;
    // RGA into the slot's tensor, then submit to the pool.
    void frame_dispatch( int slot, int src, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms );
    void record_frame_perf( int src, std::chrono::steady_clock::time_point tp ) noexcept;
    void record_orient_perf( int src, double elapsed_ms ) noexcept;
    void finish_step_perf( std::chrono::steady_clock::time_point t0 ) noexcept;
    void maybe_log_perf( std::chrono::steady_clock::time_point now ) noexcept;
    void log_src_perf() noexcept;
    void log_enc_perf() noexcept;

private:
    Config cfg_;
    UsbHost usb_host_;
    UsbNodeLocator locator_;  // attaching thread only: the ctor, then the SourceProber of run()
    SourceSet srcs_;  // the channels and the round-robin cursor
    std::array<SourceAnalytics, kMaxSources> an_ {};

    RgaResizer rga_;  // shared, stateless: writes any source into any slot tensor
    // NPU stage: the engines (one, or three with --pool 3), their slots,
    // worker threads and the in-order result FIFO. Declared before sink_:
    // the GPU drawer imports the pool's tensors and must die first.
    NpuPool pool_;
    // Band last written into each pool tensor: a band change (another
    // source, another geometry) re-grays the bars before the RGA write.
    std::array<LbGeom, NpuPool::kMaxSlots> tensor_band_ {};
    int model_w_ = 0;
    int model_h_ = 0;  // taken from the model, not from flags
    
    // M5: per-source letterbox geometry lives in SourceChannel::geom().lb; the
    // RGA per-frame LbGeom is cross-checked against the owning source's
    // in frame_dispatch (one-time warn).
    bool lb_warned_ = false;
    CalDumper cal_dumper_;
    int reconnects_ = 0;   // source disconnects recovered this run
    Timings t_;            // avg/60 + whole-run TOTAL; logs through log_

    OverlaySink sink_;
    ConsoleReporter rep_;  // throttled reports + final summary
    TrajList trajs_;
    std::optional<ReidWorker> reid_worker_ {};
    GlobalTrackTable global_tracks_ {};

    // Per-frame buffers: preallocated members, reused each frame --
    // zero heap traffic in the steady-state path.
    DetList dets_;       // smoothed detections of the frame being published
    InferResult res_;    // the pool result being published

    const StopFlag& stop_;  // set by SignalWaiter (SIGINT/SIGTERM), read only here
    const Log& log_;        // diagnostics sink (main.cpp); safe from the worker and prober threads
    std::chrono::steady_clock::time_point t0_ {};  // run() start: the pts base of live frames and NO SIGNAL
    int64_t last_idle_us_ = -1;  // last NO SIGNAL publish stamp (us since t0_)
    std::array<SourcePerfWindow, kMaxSources> src_perf_ {};
    std::array<std::chrono::steady_clock::time_point, kMaxSources> last_frame_tp_ {};
    std::array<bool, kMaxSources> has_last_frame_ {};
    std::array<uint64_t, kMaxSources> prev_skip_parts_ {};
    double step_ms_max_ = 0.0;
    std::chrono::steady_clock::time_point perf_win_start_ {};
};
