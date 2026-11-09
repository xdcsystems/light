#pragma once
#include <string>

#include "config/option_registry.h"

// Config: the complete parameter set of one run. Source dimensions (camera
// G_FMT or network JPEG SOF) and model input dimensions (model input_attr)
// are read from the source and model themselves, so --w/--h and --mw/--mh
// are intentionally absent.
struct Config
{
    std::string device = "/dev/video20";
    // Network MJPEG pull source (http://host:port/path).
    // Non-empty excludes --device: exactly one source per run.
    std::string net_src;
    std::string model = "./misc/yolov5s_relu_int8_mmse_v5.rknn";
    // Two-pass RGA kicks in only when the source exceeds 8x the model input
    // per axis (checked in the Pipeline ctor); at 720p -> 640 it stays dormant.
    bool dump_cal = false;  // dump one model-input frame to /tmp/cal.ppm
    // Calibration dataset: --cal N saves up to N model-input frames into
    // dataset/ (one every --cal-interval frames) for int8 quantization.
    int cal_frames = 0;
    int cal_interval = 15;

    // 0 (the default) = run until Ctrl-C: the SIGINT/SIGTERM handler
    // sets the stop flag, run() drains and reports normally. An explicit
    // --frames N (>= 1) bounds the run; values < 1 are rejected loudly
    // in cli.cpp.
    int frames = 0;
    // Detection thresholds, CLI-tunable via --conf / --iou.
    float conf_thr = 0.25f;
    float iou_thr = 0.45f;
    // NPU core selection: 0 = AUTO (runtime default, no call),
    // 1/2/4 = core 0/1/2, 3 = cores 0+1, 7 = cores 0+1+2
    // (rknn_set_core_mask). Ignored when pool > 1 (the pool pins its own).
    int core = 0;
    // Context pool (CLI accepts 0|1|3):
    // 0/1   = ring of one (single engine, synchronous semantics);
    //         (2 is rejected by the CLI; Pipeline maps <3 to 1)
    // 3     = three engines on one model, pinned one per NPU core,
    //         inference moved to worker threads and overlapped with the rest.
    int pool = 0;

    // Model input size is NOT a config option: it is read from the model
    // itself (input_attr) after setup_io. A --mw/--mh flag could only
    // desynchronize RGA from the real tensor geometry.

    // Raw preview server (RGB888 tensor -> NUT over TCP).
    bool net = false;
    int net_port = 1234;
    // Pacing of the preview stream, FPS. 0 = follow the source: the
    // encoder stamps pts from the dequeue times (VFR), night ~25, day
    // up to 30 -- no ctx rebuild, no fixed 25 ceiling.
    int net_fps = 25;
    // h264_rkmpp + MPEG-TS instead of raw NUT. --net-h264.
    bool net_h264 = false;
    int net_bitrate = 12000;  // kbps, --net-bitrate

    // Per-frame box CSV for the jitter A/B (raw vs smoothed).
    std::string box_log;
    // --cal-dir: overrides the --cal series directory (default: dataset/).
    std::string cal_dir;
    std::string net_host;  // UDP push destination (--net-host); empty = TCP pull on net_port
    // Force the CPU box-drawing path (A/B vs the GPU drawer).
    bool cpu_draw = false;
};

// CliOptions: the single place that reads and validates the command
// line and the INI config file (externals/inih, BSD-3-Clause). One
// registry of names, ranges and error texts (OptionRegistry);
// the config-file layer reuses the same registry, so both sources
// validate identically. parse() is called once from main();
// afterwards config() hands over a frozen, validated Config.
class CliOptions
{
public:
    enum class ParseStatus
    {
        Run,    // argv + config file validated; proceed to the Pipeline
        Help,   // --help/-h printed; main() exits 0
        Error,  // loud rejection; main() exits 2
    };

    ParseStatus parse( int argc, char** argv );
    const Config& config() const { return cfg_; }

private:
    int validate() const;  // cross-field checks; 0 = ok, 2 = loud rejection

private:
    OptionRegistry reg_;
    Config cfg_;
};
