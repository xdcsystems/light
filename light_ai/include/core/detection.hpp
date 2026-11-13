// Detection: the domain type produced by the YOLO postprocess and consumed
// by the smoother, tracker, box mapper and console reporter. No SDK
// dependencies (moved out of yolo_postprocess.hpp so consumers stop
// including the postprocess layer).

#pragma once

#include <ostream>

#include "pod_list.hpp"

struct Detection
{
    float x, y, w, h;  // scene coordinates (pixels, top-left corner)
    int cls;           // COCO class id
    float conf;        // obj * best-class probability
};

// Writes the Detection fields in the CSV order used by the box
// smoother log: cls,conf,x,y,w,h (comma-separated, no prefix and
// no newline -- the caller supplies the row prefix and terminator).
inline std::ostream& operator<<( std::ostream& os, const Detection& d )
{
    return os << d.cls << ',' << d.conf << ',' << d.x << ',' << d.y
              << ',' << d.w << ',' << d.h;
}

using DetList = PodList<Detection, 128>;
