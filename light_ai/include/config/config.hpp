// Config: the complete, validated parameter set of one run, grouped by the
// component that consumes it. Each consumer takes its own section by const
// reference, so a signature states exactly what the component depends on:
//   sources        -> Pipeline (source slots)
//   InferConfig    -> NPU pool and YOLO postprocess
//   CalConfig      -> CalDumper
//   StreamConfig   -> OverlaySink (preview/encoder, box drawing)
// Filled by CliOptions (argv + INI file through one OptionRegistry) and
// frozen afterwards. Plain data: no SDK dependencies, no option-parsing code.
//
// Source dimensions (camera G_FMT or network JPEG SOF) and model input
// dimensions (model input_attr) are read from the source and the model
// themselves, so --w/--h and --mw/--mh are intentionally absent: such a
// flag could only desynchronize RGA from the real tensor geometry.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class SourceKind : uint8_t
{
    V4L2,
    NetMjpeg
};

struct SourceEndpoint
{
    SourceKind kind = SourceKind::V4L2;
    std::string uri;  // V4L2 node (/dev/videoN) or http://host:port/path
};

struct InferConfig
{
    std::string model = "./misc/yolov5s_relu_int8_mmse_v5.rknn";  // --model
    // Detection thresholds, CLI-tunable via --conf / --iou.
    float conf_thr = 0.25f;
    float iou_thr = 0.45f;
    // NPU core selection (--core): 0 = AUTO (runtime default, no call),
    // 1/2/4 = core 0/1/2, 3 = cores 0+1, 7 = cores 0+1+2
    // (rknn_set_core_mask). Ignored when pool > 1 (the pool pins its own).
    int core = 0;
    // Context pool (--pool, the CLI accepts 0|1|3):
    // 0/1   = ring of one (single engine, synchronous semantics);
    //         (2 is rejected by the CLI; Pipeline maps <3 to 1)
    // 3     = three engines on one model, pinned one per NPU core,
    //         inference moved to worker threads and overlapped with the rest.
    int pool = 0;
};

struct CalConfig
{
    bool dump_one = false;  // --dump-cal: dump one model-input frame to /tmp/cal.ppm
    // Calibration dataset: --cal N saves up to N model-input frames
    // (one every --cal-interval frames) for int8 quantization.
    int frames = 0;
    int interval = 15;
    std::string dir;  // --cal-dir: overrides the series directory (default: dataset/)
};

struct StreamConfig
{
    bool enabled = false;  // --net: preview server (raw rgb24 NUT, or h264 with --net-h264)
    int port = 1234;       // --net-port
    // Pacing of the preview stream, FPS (--net-fps). 0 = follow the source:
    // the encoder stamps pts from the dequeue times (VFR), night ~25, day
    // up to 30 -- no ctx rebuild, no fixed 25 ceiling.
    int fps = 25;
    bool h264 = false;         // --net-h264: h264_rkmpp + MPEG-TS instead of raw NUT
    int bitrate_kbps = 12000;  // --net-bitrate
    std::string host;          // --net-host: UDP push destination; empty = TCP pull on port
    bool cpu_draw = false;     // --cpu-draw: force the CPU box-drawing path (A/B vs the GPU drawer)
};

struct Config
{
    // Ordered list of all configured video sources (M5 multi-camera).
    // Never empty after CliOptions::parse(): the default is one V4L2
    // source on /dev/video20. The single owner of "which sources" --
    // there are no separate primary-device / primary-url fields.
    std::vector<SourceEndpoint> sources;
    InferConfig infer;
    CalConfig cal;
    StreamConfig stream;

    // 0 (the default) = run until Ctrl-C: SignalWaiter sets the stop
    // flag, run() drains and reports normally. An explicit --frames N
    // (>= 1) bounds the run; values < 1 are rejected loudly in cli.cpp.
    int frames = 0;
    // Per-frame box CSV for the jitter A/B (raw vs smoothed), --box-log.
    std::string box_log;
};
