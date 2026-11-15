#pragma once

#include "core/detection.hpp"     // DetList
#include "core/log.hpp"
#include "core/yolo_decoder.hpp"  // YoloDecoder

class RknnEngine;
struct InferJob;

// The postprocess step of the NPU pool (InferencePool<RknnEngine, YoloDecode>):
// the RKNN side of YoloDecoder (core/yolo_decoder.hpp). It turns the engine
// outputs the worker just ran into YoloHead views (the first three; a head
// without data or with dims other than 4 is passed as unusable), takes the
// job's source geometry and band and the engine's own model input size, and
// lets the decoder do the rest.
// One copy per worker (InferencePool): the decoder's candidate buffer is a
// member, so one instance is never called concurrently.
class YoloDecode
{
public:
    YoloDecode( float conf_thr, float iou_thr, const Log& log ) : dec_( conf_thr, iou_thr, log ) {}
    void operator()( const RknnEngine& npu, const InferJob& job, DetList& out );

private:
    YoloDecoder dec_;
};
