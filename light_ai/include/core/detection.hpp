// Detection: the domain type produced by the YOLO postprocess and consumed
// by the smoother, tracker, box mapper and console reporter. No SDK
// dependencies (moved out of yolo_postprocess.hpp so consumers stop
// including the postprocess layer).

#pragma once

#include <cmath>
#include <ostream>

#include "core/log.hpp"
#include "core/pod_list.hpp"

struct Detection
{
    float x, y, w, h;  // scene coordinates (pixels, top-left corner)
    int cls;           // COCO class id
    float conf;        // obj * best-class probability
};

// A box every consumer can use: finite geometry and score, a positive
// extent (M-14). YoloDecoder::emit already drops non-positive sizes; the
// tracker, associator, smoother and mapper re-check so a direct core/ API
// caller (or a future decoder) cannot feed NaN into Kalman / IoU math.
inline bool det_usable( const Detection& d ) noexcept
{
    return std::isfinite( d.x ) && std::isfinite( d.y ) && std::isfinite( d.w ) && std::isfinite( d.h ) &&
           std::isfinite( d.conf ) && d.w > 0.0f && d.h > 0.0f;
}

// Writes the Detection fields in the CSV order used by the box
// smoother log: cls,conf,x,y,w,h (comma-separated, no prefix and
// no newline -- the caller supplies the row prefix and terminator).
inline std::ostream& operator<<( std::ostream& os, const Detection& d )
{
    return os << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y << ',' << d.w << ',' << d.h;
}

inline Log::Line& operator<<( Log::Line& os, const Detection& d )
{
    return os << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y << ',' << d.w << ',' << d.h;
}

using DetList = PodList<Detection, 128>;
