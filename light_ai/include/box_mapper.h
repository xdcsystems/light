// Scene -> tensor coordinate mapping for detection boxes and track trails.
// Sole consumer:
// OverlaySink::draw_and_publish (GPU and CPU draw paths share the same mapped batch).
// Colors live in box_mapper.cpp;
// these declarations keep both draw paths in sync.

#pragma once

#include "gpu_box_drawer.hpp"   // GpuBox, GpuBoxList
#include "rga_resizer.h"        // LbGeom
#include "tracker.hpp"          // TrajList, TrackState, Tracker
#include "yolo_postprocess.hpp" // DetList, Detection

// Detection boxes and trajectory trails -> boxes (tensor coords).
// Trail points are sampled ring points as 3x3 px GpuBoxes,
// in the same batch as the detection boxes.
// Scene->tensor mapping is (s, lb_top) -- identical for both loops.
// Detection boxes win the VBO budget (GpuBoxList::cap).

void fill_gpu_boxes( const DetList& dets, const TrajList& trajs, const Tracker& tracker,
                     GpuBoxList& boxes, const LbGeom& lb, float sx, float sy );

