#pragma once

#include "pod_list.hpp"
#include <ostream>

class RknnEngine;

struct Detection
{
    float x, y, w, h;  // scene coordinates (pixels, top-left corner)
    int cls;           // COCO class id
    float conf;        // obj * best-class probability
};

// Writes the Detection fields in the CSV order used by the box
// smoother log: cls,conf,x,y,w,h (comma-separated, no prefix and
// no newline - the caller supplies the row prefix and terminator).
inline std::ostream& operator<<( std::ostream& os, const Detection& d )
{
    return os << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y
              << ',' << d.w << ',' << d.h;
}

// Decodes the three yolov5 heads (fp16 NCHW, post-sigmoid) of the engine,
// applies conf threshold + NMS, filters bar detections and maps the rest
// into scene coordinates. Letterbox: the visible band occupies rows
// [lb_top, lb_top + lb_h) (landscape) or columns [lb_left, lb_left + lb_w)
// (portrait) of the model input; the mapping to scene coordinates is
// axial: scene = (det - band origin) * cam / band size.
using DetList = PodList<Detection, 128>;

void yolo_postprocess( const RknnEngine& npu,
                       float cam_w, float cam_h,
                       float model_w, float model_h,
                       float lb_left, float lb_top, float lb_w, float lb_h,
                       DetList& out,
                       float conf_thr = 0.25f,
                       float iou_thr = 0.45f );
