#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "box_smoother.h"
#include "cal_dumper.hpp"
#include "camera_v4l2.hpp"
#include "config/config.hpp"
#include "console_reporter.h"
#include "core/log.hpp"
#include "core/stop_flag.hpp"
#include "frame_trace.h"
#include "inference_pool.hpp"
#include "media_source.hpp"
#include "mpp_decoder.hpp"
#include "net_mjpeg_source.hpp"
#include "overlay_sink.hpp"
#include "rga_resizer.h"
#include "rknn_engine.hpp"
#include "timings.hpp"
#include "tracker.hpp"
#include "yolo_postprocess.hpp"

class Pipeline
{
    enum { kMaxSources = 3 };
    using NpuPool = InferencePool<RknnEngine, YoloDecode>;

    struct SourceSlot
    {
        // hot-attach (README): slot stays configured while the source is away;
        // probe_loop() flips it after a successful bring-up (probe_source)
        std::atomic<bool> absent = false;
        SourceKind kind = SourceKind::V4L2;  // survives teardown for the probe path
        MediaSource src {};
        // M5: everything below is per-source. The decoder owns its
        // out_frame_/resolution and its detected 4:2:0 vs 4:2:2 format --
        // alternating heterogeneous cameras no longer reset each other
        // (the old shared dec_ info_change resets). Smoother/tracker keep
        // per-camera state; lb is each source's own letterbox geometry.
        MppDecoder dec {};
        BoxSmoother smoother {};
        Tracker tracker {};
        LbGeom lb {};
        int aux_fd = -1;  // per-source RGA intermediate (decoder DRM pool)
        std::string node;
        int w = 0;
        int h = 0;
        int frames = 0;
        int warmup_left = 0;
        // Atomic: probe_source writes it on the prober thread; the pipeline
        // thread reads and writes it in try_dequeue_round and in run()'s
        // recovery loop -- with no common lock a plain time_point member was
        // a data race (UB). Trivially copyable, 8 B: lock-free on aarch64.
        std::atomic<Timings::clk::time_point> last_ok_tp {};
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
    void init_source();
    void init_one_source( int idx, const SourceEndpoint& ep );
    void start_source( int idx );
    void probe_source( int idx );  // hot-attach: one (re)bring-up step, never throws
    void probe_loop();
    // busy-node reservation (M5): the /dev nodes of the OTHER V4L2 slots,
    // live or absent alike -- node discovery never trials or adopts them.
    std::vector<std::string> busy_v4l2_nodes( int self_idx ) const;
    void init_aux();

    void drain_done_slots( int& ok );
    int publish_oldest();                // collect -> analytics -> output; 1 if the frame was valid
    int try_dequeue_round( int wait_ms, uint32_t& sz, int& cam_err );
    int dequeue_next_source( uint32_t& sz, int& cam_err, int& ok );

    // NO SIGNAL idle publish: gray plate tiles for absent sources at a
    // fixed pace; not counted in --frames or the timing stats.
    void idle_tick();
    int handle_source_failure( int cam_err );  // 0 = recovered, -1 = fatal
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop

    // RGA into the slot's tensor, then submit to the pool.
    void frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us,
                         double cam_ms, double dec_ms );

private:
    Config cfg_;
    std::array<SourceSlot, kMaxSources> srcs_ {};
    int nsrc_ = 1;
    int cur_src_ = 0;

    RgaResizer rga_;  // shared, stateless: writes any source into any slot tensor
    // NPU stage: the engines (one, or three with --pool 3), their slots,
    // worker threads and the in-order result FIFO. Declared before sink_:
    // the GPU drawer imports the pool's tensors and must die first.
    NpuPool pool_;
    // Band last written into each pool tensor: a band change (another
    // source, another geometry) re-grays the bars before the RGA write.
    std::array<LbGeom, NpuPool::kMaxSlots> tensor_band_ {};
    std::atomic<int> src_w_ { 0 };
    std::atomic<int> src_h_ { 0 };  // MAX across sources (init_aux sizing + diagnostics);
                                    // written by probe_source on the prober thread, hence atomic
                                    // per-source geometry is SourceSlot::w/h
    int model_w_ = 0;
    int model_h_ = 0;  // taken from the model, not from flags
    
    // M5: per-source letterbox geometry lives in SourceSlot::lb; the
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
    long long scan_log_ms_ = 0;  // last camera-scan log stamp: throttles discovery spam (30 s)
    std::chrono::steady_clock::time_point t0_ {};  // idle pts base (NO SIGNAL timeline)
    int64_t last_idle_us_ = -1;  // last NO SIGNAL publish stamp (us since t0_)
};
