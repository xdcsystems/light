#pragma once

#include "core/detection.hpp"  // Detection, DetList

// LbGeom (core/geometry.hpp): the letterbox band travels as one object;
// forward-declared -- the header declares only, the cpp maps the fields.
class RknnEngine;
struct LbGeom;

// Decodes the three yolov5 heads (fp16 NCHW, post-sigmoid) of the engine,
// applies conf threshold + NMS, filters bar detections and maps the rest
// into scene coordinates. Letterbox: the visible band occupies rows
// [lb.top, lb.top + lb.h) (landscape) or columns
// [lb.left, lb.left + lb.w) (portrait) of the model input;
// the mapping to scene coordinates is axial:
// scene = (det - band origin) * cam / band size.
void yolo_postprocess( const RknnEngine& npu,
                       float cam_w, float cam_h,
                       float model_w, float model_h,
                       const LbGeom& lb,
                       DetList& out,
                       float conf_thr = 0.25f,
                       float iou_thr = 0.45f );
