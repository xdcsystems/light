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
#include <iostream>
#include <variant>
#include <chrono>   // Timings: steady_clock stamps
#include <iomanip>  // Timings: report formatting

#include "config/cli.hpp"  // Config (moved out of this header 2026-11-08)
#include "camera_v4l2.hpp"
#include "yolo_postprocess.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"
#include "cal_dumper.hpp"
#include "net_preview.hpp"
#include "net_encoder.hpp"
#include "gpu_box_drawer.hpp"
#include "box_smoother.hpp"
#include "tracker.hpp"
#include "console_reporter.hpp"
#include "slot_ring.hpp"
#include "frame_trace.hpp"
#include "net_mjpeg_source.hpp"

class Pipeline
{
    // Timing accumulator: the throttled avg/60 + TOTAL reports.
    // Shared mutable state lives in the owning layer (project rule):
    // a Pipeline member, not an anonymous-namespace global.
    struct Timings
    {
        using clk = std::chrono::steady_clock;

        double cam = 0, dec = 0, rga = 0, infer = 0, enc = 0;  // sums, ms
        double post = 0.0;
        int n = 0;
        // Pool accounting: in pool mode the per-stage sums double-count
        // the in-flight window overlap (cam wait of frame k vs infer of
        // frame k-2), so sum -> FPS is wrong; FPS comes from the wall
        // clock of the whole run instead. CFR (pool 1) keeps the legacy
        // honest accounting.
        clk::time_point t0{};  // first frame stamp
        long total = 0;        // frames since start
        int pool = 1;          // in-flight frames; 1 = legacy path

        void set_pool( int p )
        {
            if ( p > 1 )
                pool = p;
        }

        void add( const FrameTrace& tr )
        {
            if ( total == 0 )
                t0 = clk::now();
            ++total;
            cam += tr.cam;
            dec += tr.dec;
            rga += tr.rga;
            infer += tr.infer;
            post += tr.post;
            enc += tr.enc;
            if ( ++n % 60 == 0 )
                report( "avg/60" );
        }

        void report( const char* tag )
        {
            if ( n == 0 )
                return;
            const double sum = ( cam + dec + rga + infer + post + enc ) / n;
            if ( pool > 1 )
            {
                const double wall = total / std::chrono::duration<double>( clk::now() - t0 ).count();
                std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                          << " rga=" << rga / n << " infer=" << infer / n << " post=" << post / n << " enc=" << enc / n << " | sum=" << sum
                          << " ms (pool window latency, " << pool << " in flight) | wall ~" << wall << " FPS\n";
            }
            else
            {
                std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                          << " rga=" << rga / n << " infer=" << infer / n << " post=" << post / n << " enc=" << enc / n << " | sum=" << sum << " ms -> ~"
                          << ( sum > 0 ? 1000.0 / sum : 0.0 ) << " FPS\n";
            }
            cam = dec = rga = infer = post = enc = 0;
            n = 0;
        }
    };

public:
    explicit Pipeline( const Config& cfg );
    ~Pipeline();  // stops the workers before the engines die
    int run();
    // Graceful shutdown (P1-6): called from the SIGINT/SIGTERM handler;
    // run() exits the frame loop through the normal drain/summary path.
    void request_stop() { stop_.store( true ); }

private:
    bool use_net_src() const { return !cfg_.net_src.empty(); }

    // Static source selection (project rules: no virtuals/vtables): the
    // active source is a pointer variant over the member objects;
    // with_src() is the single dispatch point. M5: a third source type
    // adds one alternative — call sites stay untouched.
    template <typename F>
    decltype(auto) with_src( F&& f )
    {
        return std::visit( [&f]( auto* s ) -> decltype(auto) { return f( *s ); }, src_ );
    }

    // Source bring-up (identity + setup + variant selection), extracted
    // from the ctor: with the net source branch the ctor crossed the
    // readability-function-size limit (225 lines / threshold 200).
    void init_source();
    void init_net_sink( const rknn_tensor_attr& ia );
    void init_gpu_drawer();
    void init_aux();
    void setup_cores();

    void worker_loop( int slot );
    int publish_oldest();                // returns 1 if the frame was valid
    void dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, const LbGeom& lb,
            double cam_ms, double dec_ms, double rga_ms );
    // M6 batch-2: run()/publish_oldest() VariableThreshold splits.
    int handle_source_failure( int cam_err );  // 0 = recovered, -1 = fatal
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop
    void frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us,
            double cam_ms, double dec_ms );
    double draw_and_publish( Slot& sl, int ps );   // viewer path; returns enc_ms
    void pool_discard();                 // drop pending slots on source recovery

private:
    Config cfg_;
    CameraV4L2 cam_;
    std::string cam_node_;

    MppDecoder dec_;
    RgaResizer rga_;
    // M4: context pool. npus_[0] IS the pool-1 engine;
    // --pool 3 loads the model three times, one engine per NPU core.
    // The vector never reallocates after the ctor, so &npus_[k] stays
    // valid for the whole session.
    std::vector<RknnEngine> npus_;
    int npool_ = 1;
    int aux_fd_ = -1;
    int model_w_ = 0;
    int model_h_ = 0;  // taken from the model, not from flags
    // Audit item 3: single-source letterbox geometry. Computed once in
    // the ctor via band_geom and fed to BOTH consumers —
    // the encoder crop (NetEncoder ctor) and the postprocess scale
    // (dispatch). RGA stays the pixel writer; its per-frame LbGeom is
    // cross-checked against this one at dispatch (one-time warn).
    LbGeom lb_ {};
    bool lb_warned_ = false;
    CalDumper cal_dumper_;  // cal_saved_/last_cal_/cal_done_ moved here (stage 2)
    int reconnects_ = 0;   // source disconnects recovered this run
    int warmup_left_ = 0;  // publish-discards after a reconnect
    Timings t_;

    std::optional<NetMjpegSource> net_src_;  // active only with --net-src
    std::optional<NetPreview> preview_;
    std::optional<NetEncoder> enc_;
    std::optional<GpuBoxDrawer> drawer_;

    std::variant<CameraV4L2*, NetMjpegSource*> src_ {};

    BoxSmoother smoother_;  // M3.10 EMA box smoothing + jitter log
    Tracker tracker_;
    ConsoleReporter rep_;  // throttled reports + final summary (stage 3)
    TrajList trajs_;

    // Per-frame detection buffers: preallocated members, cleared each
    // frame — zero heap traffic in the steady-state path.
    DetList dets_;
    GpuBoxList boxes_;

    std::array<Slot, 3> slots_ {};
    std::vector<std::thread> workers_;   // one per pooled engine
    std::atomic<bool> quit_ { false };
    std::atomic<bool> stop_ { false };  // P1-6: set by the signal handler
    PubFifo pub_;
};
