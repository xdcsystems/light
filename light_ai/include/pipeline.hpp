#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "box_smoother.h"
#include "cal_dumper.hpp"
#include "config/config.hpp"
#include "console_reporter.h"
#include "core/log.hpp"
#include "core/stop_flag.hpp"
#include "frame_trace.h"
#include "inference_pool.hpp"
#include "overlay_sink.hpp"
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
    enum { kMaxSources = 3 };
    using NpuPool = InferencePool<RknnEngine, YoloDecode>;
    using SourceSet = BasicSourceSet<SourceChannel, kMaxSources>;
    using SourceProber = BasicSourceProber<SourceSet, UsbNodeLocator>;

    // Outcome of one frame-loop step (step()).
    enum class Step : uint8_t
    {
        Dispatched,  // a frame went to the pool: counts against --frames
        Skipped,     // nothing to count: empty packet, decode error, warm-up, a handled detach
        Stop,        // stop requested: leave through the drain/summary path
        Fatal        // unrecoverable source error: run() returns -1 (exit code 1)
    };

    // Per-source analytics, frame thread only (publish_oldest): the
    // source itself -- device, decoder, geometry, state -- is SourceChannel.
    struct SourceAnalytics
    {
        BoxSmoother smoother {};
        Tracker tracker {};
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
    // the stop flag is set (graceful shutdown, P1-6).
    int run();

private:
    void attach_sources();  // ctor, last: the first attach of every channel (STREAMON)

    // One frame: next_frame -> decode -> dispatch -> publish (pool 1).
    Step step( int& ok );
    // The next encoded frame or a failure; parks (NO SIGNAL, 40 ms quanta)
    // while no source is Live. Stopped once the stop flag is set.
    FrameStatus next_frame( EncodedFrame& f, int& ok );
    void drain_done_slots( int& ok );
    int publish_oldest();                // collect -> analytics -> output; 1 if the frame was valid

    // NO SIGNAL idle publish: gray plate tiles for absent sources at a
    // fixed pace; not counted in --frames or the timing stats.
    void idle_tick();
    int handle_source_failure( const EncodedFrame& f );  // 0 = recovered, -1 = fatal
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop

    // RGA into the slot's tensor, then submit to the pool.
    void frame_dispatch( int slot, int src, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms );

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

    // Per-frame buffers: preallocated members, reused each frame --
    // zero heap traffic in the steady-state path.
    DetList dets_;       // smoothed detections of the frame being published
    InferResult res_;    // the pool result being published

    const StopFlag& stop_;  // set by SignalWaiter (SIGINT/SIGTERM), read only here
    const Log& log_;        // diagnostics sink (main.cpp); safe from the worker and prober threads
    std::chrono::steady_clock::time_point t0_ {};  // run() start: the pts base of live frames and NO SIGNAL
    int64_t last_idle_us_ = -1;  // last NO SIGNAL publish stamp (us since t0_)
};
