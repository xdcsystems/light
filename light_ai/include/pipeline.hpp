#pragma once
#include <string>
#include <memory>
#include "camera_v4l2.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"

struct Config {
    std::string device  = "/dev/video20";
    std::string model   = "/usr/share/model/RK3588/mobilenet_v1.rknn";
    int cam_w = 640, cam_h = 480;
    int model_w = 224, model_h = 224;   // под модель (для yolov8n: 640x640)
    int frames = 300;
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);
    int run();

private:
    Config cfg_;
    std::unique_ptr<CameraV4L2> cam_;
    std::unique_ptr<MppDecoder> dec_;
    RgaResizer rga_;
    std::unique_ptr<RknnEngine> npu_;
    int npu_input_fd_ = -1;
};
