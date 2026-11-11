#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "box_smoother.h"
#include "cal_dumper.hpp"
#include "camera_v4l2.hpp"
#include "config/cli.hpp"
#include "console_reporter.h"
#include "frame_trace.h"
#include "media_source.hpp"
#include "mpp_decoder.hpp"
#include "net_mjpeg_source.hpp"
#include "overlay_sink.hpp"
#include "rga_resizer.h"
#include "rknn_engine.hpp"
#include "slot_ring.hpp"
#include "timings.hpp"
#include "tracker.hpp"
#include "yolo_postprocess.hpp"

class Pipeline
{
    enum { kMaxSources = 3 };

    struct SourceSlot
    {
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
        Timings::clk::time_point last_ok_tp {};
    };

public:
    explicit Pipeline( const Config& cfg );
    ~Pipeline();  // stops the workers before the engines die
    Pipeline( const Pipeline& ) = delete;
    Pipeline& operator=( const Pipeline& ) = delete;
    Pipeline( Pipeline&& ) = delete;
    Pipeline& operator=( Pipeline&& ) = delete;

    int run();
    // Graceful shutdown (P1-6): called from the SIGINT/SIGTERM handler;
    // run() exits the frame loop through the normal drain/summary path.
    void request_stop() { stop_.store( true ); }

private:
    void init_source();
    void init_one_source( int idx, const SourceEndpoint& ep );
    void start_source( int idx );
    void init_aux();
    void setup_cores();

    void worker_loop( int slot );
    bool front_slot_done();
    void drain_done_slots( int& ok );
    int publish_oldest();                // returns 1 if the frame was valid
    void pool_discard( int src_idx );    // drop pending slots of src_idx on recovery
    int try_dequeue_round( int wait_ms, uint32_t& sz, int& cam_err );
    int dequeue_next_source( uint32_t& sz, int& cam_err, int& ok );
    int handle_source_failure( int cam_err );  // 0 = recovered, -1 = fatal
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop

    void dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, const LbGeom& lb,
                   double cam_ms, double dec_ms, double rga_ms );

    void frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us,
                         double cam_ms, double dec_ms );

private:
    Config cfg_;
    std::array<SourceSlot, kMaxSources> srcs_ {};
    int nsrc_ = 1;
    int cur_src_ = 0;

    RgaResizer rga_;  // shared, stateless: writes any source into any slot tensor
    // Context pool: npus_[0] IS the pool-1 engine;
    // --pool 3 loads the model three times, one engine per NPU core.
    // The vector never reallocates after the ctor,
    // so &npus_[k] stays valid for the whole session.
    std::vector<RknnEngine> npus_;
    int npool_ = 1;
    int src_w_ = 0;
    int src_h_ = 0;    // MAX across sources (init_aux sizing + diagnostics);
                       // per-source geometry is SourceSlot::w/h
    int model_w_ = 0;
    int model_h_ = 0;  // taken from the model, not from flags
    
    // M5: per-source letterbox geometry lives in SourceSlot::lb; the
    // RGA per-frame LbGeom is cross-checked against the owning source's
    // at dispatch (one-time warn).
    bool lb_warned_ = false;
    CalDumper cal_dumper_;
    int reconnects_ = 0;   // source disconnects recovered this run
    Timings t_;

    OverlaySink sink_ {};
    ConsoleReporter rep_;  // throttled reports + final summary
    TrajList trajs_;

    // Per-frame detection buffer: preallocated member, cleared each
    // frame -- zero heap traffic in the steady-state path.
    DetList dets_;

    std::array<Slot, 3> slots_ {};
    std::vector<std::thread> workers_;   // one per pooled engine
    std::atomic<bool> quit_ { false };
    std::atomic<bool> stop_ { false };  // set by the signal handler
    PubFifo pub_;
};
