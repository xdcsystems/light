#pragma once
#include <string>
#include "camera_v4l2.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"

struct Config {
    std::string device = "/dev/video20";
    std::string model  = "/usr/share/model/RK3588/mobilenet_v1.rknn";
    //int cam_w = 1920, cam_h = 1080;
    int cam_w = 1280, cam_h = 720;
    // Two-pass RGA kicks in only when the source exceeds 8x the model input
    // per axis (checked in the Pipeline ctor); at 720p -> 640 it stays dormant.
    bool dump_cal = false;   // dump one model-input frame to /tmp/cal.ppm
    // Calibration dataset: --cal N saves up to N model-input frames into
    // dataset/ (one every --cal-interval frames) for int8 quantization.
    int cal_frames = 0;
    int cal_interval = 15;

    int frames = 300;
    // Detection thresholds, CLI-tunable via --conf / --iou.
    float conf_thr = 0.25f;
    float iou_thr  = 0.45f;
    // Model input size is NOT a config option: it is read from the model
    // itself (input_attr) after setup_io. A --mw/--mh flag could only
    // desynchronize RGA from the real tensor geometry.
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);
    int run();

private:
    Config cfg_;
    CameraV4L2 cam_;
    MppDecoder dec_;
    RgaResizer rga_;
    RknnEngine npu_;
    int aux_fd_ = -1;
    int model_w_ = 0, model_h_ = 0;   // taken from the model, not from flags
    bool cal_done_ = false;
    int cal_saved_ = 0;
    int last_cal_ = -1;
};
