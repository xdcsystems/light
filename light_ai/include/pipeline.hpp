#pragma once
#include <string>
#include "camera_v4l2.hpp"
#include "mpp_decoder.hpp"
#include "rga_resizer.hpp"
#include "rknn_engine.hpp"

struct Config {
    std::string device = "/dev/video20";
    std::string model  = "/usr/share/model/RK3588/mobilenet_v1.rknn";
    int cam_w = 640, cam_h = 480;
    int frames = 300;
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
    int model_w_ = 0, model_h_ = 0;   // taken from the model, not from flags
};
