#pragma once

#include "core/detection.hpp"  // Detection, DetList

// LbGeom (core/geometry.hpp): the letterbox band travels as one object;
// forward-declared -- the header declares only, the cpp maps the fields.
class RknnEngine;
struct LbGeom;
struct InferJob;

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

// The postprocess step of the NPU pool (InferencePool<RknnEngine, YoloDecode>):
// runs yolo_postprocess on the engine a worker just ran, with the job's
// source geometry and band and the engine's own model input size.
// Stateless apart from the thresholds: the three workers call one copy
// concurrently.
class YoloDecode
{
public:
    YoloDecode( float conf_thr, float iou_thr ) : conf_thr_( conf_thr ), iou_thr_( iou_thr ) {}
    void operator()( const RknnEngine& npu, const InferJob& job, DetList& out ) const;

private:
    float conf_thr_;
    float iou_thr_;
};
