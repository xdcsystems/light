#pragma once
#include <vector>

class RknnEngine;

struct Detection {
    float x, y, w, h;   // scene coordinates (pixels, top-left corner)
    int   cls;          // COCO class id
    float conf;         // obj * best-class probability
};

// Decodes the three yolov5 heads (fp16 NCHW, post-sigmoid) of the engine,
// applies conf threshold + NMS, filters bar detections and maps the rest
// into scene coordinates. Letterbox: the visible band occupies rows
// [lb_top, lb_top + lb_h) of the model input; scene scale is uniform by
// width (cam_w / model_w).
std::vector<Detection> yolo_postprocess(const RknnEngine& npu,
                                        float cam_w, float cam_h,
                                        float model_w, float model_h,
                                        float lb_top, float lb_h,
                                        float conf_thr = 0.25f,
                                        float iou_thr = 0.45f);
