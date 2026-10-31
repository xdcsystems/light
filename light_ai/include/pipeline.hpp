#pragma once
#include <optional>
#include <string>
#include "camera_v4l2.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"
#include "net_preview.hpp"
#include "net_encoder.hpp"
#include "gpu_box_drawer.hpp"
#include "box_smoother.hpp"

struct Config
{
    std::string device = "/dev/video20";
    std::string model = "./misc/yolov5s_relu_int8_mmse.rknn";
    //int cam_w = 1920, cam_h = 1080;
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
    // 1 = core 0, 3 = cores 0+1, 7 = cores 0+1+2 (rknn_set_core_mask).
    int core = 0;
    // Model input size is NOT a config option: it is read from the model
    // itself (input_attr) after setup_io. A --mw/--mh flag could only
    // desynchronize RGA from the real tensor geometry.

    // M3.5 phase A0: raw preview server (RGB888 tensor -> NUT over TCP).
    bool net = false;
    int net_port = 1234;
    int net_fps = 25;  // pacing of the preview stream, FPS
    // M3.5 phase B: h264_rkmpp + MPEG-TS instead of raw NUT. --net-h264.
    bool net_h264 = false;
    int net_bitrate = 12000;  // kbps, --net-bitrate

    // M3.10: per-frame box CSV for the jitter A/B (raw vs smoothed).
    std::string box_log;
    std::string net_host;  // UDP destination, --net-host (required with --net-h264)
    // M3.7: force the CPU box-drawing path (A/B vs the GPU drawer).
    bool cpu_draw = false;
};

class Pipeline
{
public:
    explicit Pipeline( const Config& cfg );
    int run();

private:
    Config cfg_;
    CameraV4L2 cam_;
    std::string cam_node_;
    MppDecoder dec_;
    RgaResizer rga_;
    RknnEngine npu_;
    int aux_fd_ = -1;
    int model_w_ = 0;
    int model_h_ = 0;  // taken from the model, not from flags
    bool cal_done_ = false;
    int cal_saved_ = 0;
    int last_cal_ = -1;

    std::optional<NetPreview> preview_;
    std::optional<NetEncoder> enc_;
    std::optional<GpuBoxDrawer> drawer_;

    BoxSmoother smoother_;  // M3.10 EMA box smoothing + jitter log

    // Per-frame detection buffers: preallocated members, cleared each
    // frame — zero heap traffic in the steady-state path.
    DetList dets_raw_;
    DetList dets_;
    GpuBoxList boxes_;
};
