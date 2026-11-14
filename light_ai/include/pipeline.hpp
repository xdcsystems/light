#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

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
#include "source/source_types.hpp"
#include "sources.hpp"
#include "timings.hpp"
#include "tracker.hpp"
#include "yolo_postprocess.hpp"

class Pipeline
{
    enum { kMaxSources = 3 };
    using NpuPool = InferencePool<RknnEngine, YoloDecode>;

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
    void init_source();     // validates and configures the channels (no device is opened)
    void attach_sources();  // ctor, last: the first attach of every channel (STREAMON)
    void probe_source( int idx );  // hot-attach: one (re)attach step, never throws
    void probe_loop();
    // busy-node reservation (M5): the /dev nodes of the OTHER V4L2 sources,
    // live or absent alike -- node discovery never trials or adopts them.
    NodeList busy_v4l2_nodes( int self_idx ) const;

    void drain_done_slots( int& ok );
    int publish_oldest();                // collect -> analytics -> output; 1 if the frame was valid
    // One round-robin round over the Live sources from cur_src_ + 1 (stall detector inside).
    FrameStatus try_dequeue_round( int wait_ms, EncodedFrame& f );
    FrameStatus dequeue_next_source( EncodedFrame& f, int& ok );

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
    UsbNodeLocator locator_;  // attaching thread only: the ctor, then probe_loop
    std::array<SourceChannel, kMaxSources> srcs_;
    std::array<SourceAnalytics, kMaxSources> an_ {};
    int nsrc_ = 1;
    int cur_src_ = 0;  // round-robin cursor: the source of the last dequeue attempt

    RgaResizer rga_;  // shared, stateless: writes any source into any slot tensor
    // NPU stage: the engines (one, or three with --pool 3), their slots,
    // worker threads and the in-order result FIFO. Declared before sink_:
    // the GPU drawer imports the pool's tensors and must die first.
    NpuPool pool_;
    // Band last written into each pool tensor: a band change (another
    // source, another geometry) re-grays the bars before the RGA write.
    std::array<LbGeom, NpuPool::kMaxSlots> tensor_band_ {};
    std::atomic<int> src_w_ { 0 };
    std::atomic<int> src_h_ { 0 };  // MAX across sources (diagnostics);
                                    // written by probe_source on the prober thread, hence atomic;
                                    // per-source geometry is SourceChannel::geom()
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
    std::thread probe_th_;   // hot-attach prober: armed in run(), joined on exit
    std::atomic<bool> probe_quit_ { false };
    uint64_t probe_ticks_ = 0;  // prober round counter: throttles the probe log
    std::chrono::steady_clock::time_point t0_ {};  // idle pts base (NO SIGNAL timeline)
    int64_t last_idle_us_ = -1;  // last NO SIGNAL publish stamp (us since t0_)
};
