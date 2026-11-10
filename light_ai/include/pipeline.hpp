#pragma once
#include <array>
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

#include "config/cli.hpp"
#include "camera_v4l2.hpp"
#include "yolo_postprocess.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.h"
#include "rknn_engine.hpp"
#include "cal_dumper.hpp"
#include "net_preview.hpp"
#include "net_encoder.h"
#include "gpu_box_drawer.hpp"
#include "box_smoother.h"
#include "tracker.hpp"
#include "console_reporter.h"
#include "slot_ring.hpp"
#include "frame_trace.h"
#include "timings.hpp"
#include "net_mjpeg_source.hpp"

class Pipeline
{
    enum { kMaxSources = 3 };
    using VideoSource = std::variant<CameraV4L2, NetMjpegSource>;

    struct SourceSlot
    {
        VideoSource src { std::in_place_type<CameraV4L2> };
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
        int warmup_left = 0;
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
    // Static heterogeneous source dispatch (up to kMaxSources = 3, one per NPU
    // core): each SourceSlot owns its active source (CameraV4L2 or
    // NetMjpegSource) in-place by value — no heap, no virtuals.
    template <typename F>
    decltype(auto) with_src( int idx, F&& f )
    {
        return std::visit( std::forward<F>( f ), srcs_[idx].src );
    }

    template <typename F>
    decltype(auto) with_src( F&& f )
    {
        return with_src( cur_src_, std::forward<F>( f ) );
    }

    void init_source();
    void init_one_source( int idx, const SourceEndpoint& ep );
    void start_source( int idx );
    void init_net_sink( const rknn_tensor_attr& ia );
    void init_gpu_drawer();
    void init_aux();
    void setup_cores();

    void worker_loop( int slot );
    int publish_oldest();                // returns 1 if the frame was valid
    double draw_and_publish( Slot& sl, int ps );   // viewer path; returns enc_ms
    void pool_discard();                 // drop pending slots on source recovery
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
    // at dispatch (one-time warn). The encoder band is source 0's
    // (mixed aspects: exact crop for src 0; per-frame band injection
    // into NetEncoder::publish is the follow-up).
    bool lb_warned_ = false;
    CalDumper cal_dumper_;
    int reconnects_ = 0;   // source disconnects recovered this run
    Timings t_;

    std::optional<NetPreview> preview_;
    std::optional<NetEncoder> enc_;
    std::optional<GpuBoxDrawer> drawer_;

    ConsoleReporter rep_;  // throttled reports + final summary
    TrajList trajs_;

    // Per-frame detection buffers: preallocated members, cleared each
    // frame -- zero heap traffic in the steady-state path.
    DetList dets_;
    GpuBoxList boxes_;

    std::array<Slot, 3> slots_ {};
    std::vector<std::thread> workers_;   // one per pooled engine
    std::atomic<bool> quit_ { false };
    std::atomic<bool> stop_ { false };  // set by the signal handler
    PubFifo pub_;
};
