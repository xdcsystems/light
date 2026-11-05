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
#include <iostream> // Timings: report prints

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
#include "net_mjpeg_source.hpp"

// Per-frame timing, single storage: dispatch fills cam/dec/rga,
// the worker fills infer, publish fills enc; Timings::add consumes
// the whole record.
struct FrameTrace {
    double cam = 0.0;
    double dec = 0.0;
    double rga = 0.0;
    double infer = 0.0;
    double post = 0.0;
    double enc = 0.0;
};

struct Config
{
    std::string device = "/dev/video20";
    // M5 pre-work: network MJPEG pull source (http://host:port/path).
    // Non-empty excludes --device: exactly one source per run.
    std::string net_src;
    std::string model = "./misc/yolov5s_relu_int8_mmse_v5.rknn";
    int cam_w = 1280, cam_h = 960;
    // Two-pass RGA kicks in only when the source exceeds 8x the model input
    // per axis (checked in the Pipeline ctor); at 720p -> 640 it stays dormant.
    bool dump_cal = false;  // dump one model-input frame to /tmp/cal.ppm
    // Calibration dataset: --cal N saves up to N model-input frames into
    // dataset/ (one every --cal-interval frames) for int8 quantization.
    int cal_frames = 0;
    int cal_interval = 15;

    int frames = 300;
    // Detection thresholds, CLI-tunable via --conf / --iou.
    float conf_thr = 0.25f;
    float iou_thr = 0.45f;
    // NPU core selection (M2.5): 0 = AUTO (runtime default, no call),
    // 1/2/4 = core 0/1/2, 3 = cores 0+1, 7 = cores 0+1+2
    // (rknn_set_core_mask). Ignored when pool > 1 (the pool pins its own).
    int core = 0;
    // M4: context pool. 0/1 = ring of one (single engine, synchronous
    // semantics); 3 = three engines on one model, pinned one per NPU core,
    // inference moved to worker threads and overlapped with the rest.
    int pool = 0;
    // Model input size is NOT a config option: it is read from the model
    // itself (input_attr) after setup_io. A --mw/--mh flag could only
    // desynchronize RGA from the real tensor geometry.

    // M3.5 phase A0: raw preview server (RGB888 tensor -> NUT over TCP).
    bool net = false;
    int net_port = 1234;
    // Pacing of the preview stream, FPS. 0 = follow the source: the
    // encoder stamps pts from the dequeue times (VFR), night ~25, day
    // up to 30 — no ctx rebuild, no fixed 25 ceiling.
    int net_fps = 25;
    // M3.5 phase B: h264_rkmpp + MPEG-TS instead of raw NUT. --net-h264.
    bool net_h264 = false;
    int net_bitrate = 12000;  // kbps, --net-bitrate

    // M3.10: per-frame box CSV for the jitter A/B (raw vs smoothed).
    std::string box_log;
    // --cal-dir: overrides the --cal series directory (default: dataset/).
    std::string cal_dir;
    std::string net_host;  // UDP push destination (--net-host); empty = TCP pull on net_port
    // M3.7: force the CPU box-drawing path (A/B vs the GPU drawer).
    bool cpu_draw = false;
};

// Publish FIFO: strictly single-threaded — dispatch, publish, drain
// and pool_discard all run on the pipeline thread, so no locks by
// design. Depth 3 == the maximum NPU pool size.
struct PubFifo
{
    int slots[3] = {};
    int head = 0;
    int n = 0;

    void push( int s )
    {
        // Defensive guard: the depth-3 invariant ("in pub_ live exactly
        // the slots with st != FREE; at most npool_ <= 3 of them") is
        // proven by the slot machine, but it lives in comments across
        // three functions. One refactor away from a silent corruption
        // (push would overwrite slots[head] and inflate n past front/pop
        // coherence) -- so the violation gets a loud log instead.
        // Strictly single-threaded by design (see struct comment): the
        // cerr race risk with worker output exists only if this ever
        // stops being true.
        if ( n >= 3 )
        {
            std::cerr << "pubfifo: overflow (n=" << n << ", head=" << head
                      << "), incoming slot " << s << " dropped\n";
            return;
        }
        slots[( head + n ) % 3] = s;
        ++n;
    }
    int front() const
    {
        return slots[head];
    }
    void pop()
    {
        head = ( head + 1 ) % 3;
        --n;
    }
};

class Pipeline
{
public:
    explicit Pipeline( const Config& cfg );
    ~Pipeline();  // stops the workers before the engines die
    int run();
    // Graceful shutdown (P1-6): called from the SIGINT/SIGTERM handler;
    // run() exits the frame loop through the normal drain/summary path.
    void request_stop() { stop_.store( true ); }

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
    int reconnects_ = 0;   // camera disconnects recovered this run
    int warmup_left_ = 0;  // publish-discards after a reconnect
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
    Timings t_;

    std::optional<NetMjpegSource> net_src_;  // active only with --net-src
    std::optional<NetPreview> preview_;
    std::optional<NetEncoder> enc_;
    std::optional<GpuBoxDrawer> drawer_;

    // Static source selection (project rules: no virtuals/vtables): the
    // active source is a pointer variant over the member objects;
    // with_src() is the single dispatch point. M5: a third source type
    // adds one alternative — call sites stay untouched.
    std::variant<CameraV4L2*, NetMjpegSource*> src_ {};
    template <typename F>
    decltype(auto) with_src( F&& f )
    {
        return std::visit( [&f]( auto* s ) -> decltype(auto) { return f( *s ); }, src_ );
    }

    BoxSmoother smoother_;  // M3.10 EMA box smoothing + jitter log
    Tracker tracker_;
    TrajList trajs_;

    // Per-frame detection buffers: preallocated members, cleared each
    // frame — zero heap traffic in the steady-state path.
    DetList dets_;
    GpuBoxList boxes_;

    // --- M4: context-pool machinery (active only with --pool 3) ---
    // A slot is a three-state cell: FREE -> QUEUED (main dispatches a
    // frame) -> DONE (worker finished infer + postprocess) -> FREE
    // (main published). One slot per engine; the ring depth equals the
    // pool size, publish-before-reuse keeps tensor ownership safe and
    // frame order strictly sequential (BoxSmoother sees the legacy order).
    struct Slot
    {
        RknnEngine* eng = nullptr;
        DetList dets;                    // worker-side postprocess output
        int frame = -1;                  // global frame index (logs)
        int64_t ts_us = -1;              // dequeue time, µs from start (VFR pts)
        bool rga_ok = false;             // captured at dispatch
        bool npu_ok = false;             // worker verdict, read at publish
        bool drop = false;               // publish gave up: the worker frees the slot itself
        LbGeom lb;  // letterbox geometry at dispatch (single-owner struct)
        FrameTrace tr;  // per-frame timings: dispatch->cam/dec/rga, worker->infer, publish->enc
        std::mutex mx;
        std::condition_variable cv;
        enum St : uint8_t { FREE, QUEUED, DONE } st = FREE;
    };
    std::array<Slot, 3> slots_ {};
    std::vector<std::thread> workers_;   // one per pooled engine
    std::atomic<bool> quit_ { false };
    std::atomic<bool> stop_ { false };  // P1-6: set by the signal handler
    PubFifo pub_;

    bool use_net_src() const { return !cfg_.net_src.empty(); }
    // Source bring-up (identity + setup + variant selection), extracted
    // from the ctor: with the net source branch the ctor crossed the
    // readability-function-size limit (225 lines / threshold 200).
    void init_source();
    void worker_loop( int slot );
    int publish_oldest();                // returns 1 if the frame was valid
    void dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, const LbGeom& lb,
            double cam_ms, double dec_ms, double rga_ms );
    // M6 batch-2: run()/publish_oldest() VariableThreshold splits.
    int handle_camera_failure( int cam_err );  // 0 = recovered, -1 = fatal
    int acquire_slot( int& ok );               // claim-or-drain; idx or -1 on stop
    void frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us,
            double cam_ms, double dec_ms );
    void fill_gpu_boxes( float sx, float sy, float lb_left, float lb_top );  // det boxes + trails -> boxes_
    double draw_and_publish( Slot& sl, int ps );   // viewer path; returns enc_ms
    void pool_discard();                 // drop pending slots on camera recovery
};
