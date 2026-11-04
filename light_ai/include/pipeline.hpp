#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "camera_v4l2.hpp"
#include "yolo_postprocess.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"
#include "net_preview.hpp"
#include "net_encoder.hpp"
#include "gpu_box_drawer.hpp"
#include "box_smoother.hpp"

// Per-frame timing, single storage: dispatch fills cam/dec/rga,
// the worker fills infer, publish fills enc; Timings::add consumes
// the whole record.
struct FrameTrace {
    double cam = 0.0;
    double dec = 0.0;
    double rga = 0.0;
    double infer = 0.0;
    double enc = 0.0;
};

struct Config
{
    std::string device = "/dev/video20";
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
    // the ctor via RgaResizer::band_geom and fed to BOTH consumers —
    // the encoder crop (NetEncoder ctor) and the postprocess scale
    // (dispatch). RGA stays the pixel writer; its per-frame LbGeom is
    // cross-checked against this one at dispatch (one-time warn).
    LbGeom lb_ {};
    bool lb_warned_ = false;
    bool cal_done_ = false;
    int cal_saved_ = 0;
    int last_cal_ = -1;

    std::optional<NetPreview> preview_;
    std::optional<NetEncoder> enc_;
    std::optional<GpuBoxDrawer> drawer_;

    BoxSmoother smoother_;  // M3.10 EMA box smoothing + jitter log

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
        float lb_top = 0, lb_h = 0;      // letterbox geometry at dispatch
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

    void worker_loop( int slot );
    int publish_oldest();                // returns 1 if the frame was valid
    void dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, int lb_top, int lb_h,
            double cam_ms, double dec_ms, double rga_ms );
    void cal_dump_maybe( RknnEngine& e, int frame_idx );  // shared by both paths
    void pool_discard();                 // drop pending slots on camera recovery
};
