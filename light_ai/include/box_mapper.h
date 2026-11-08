// Box mapping layer, extracted from pipeline.cpp (stage 7):
// scene -> tensor coordinates for detection boxes and track trails.
// One consumer: Pipeline::draw_and_publish (GPU and CPU draw paths
// share the same mapped batch). Colors live in box_mapper.cpp; the
// declarations here keep both draw paths in sync.

#pragma once

#include "gpu_box_drawer.hpp"   // GpuBox, GpuBoxList
#include "tracker.hpp"          // TrajList, TrackState, Tracker
#include "yolo_postprocess.hpp" // DetList, Detection

// Detection boxes and trajectory trails -> boxes (tensor coords).
// M3.11: trail points are sampled ring points as 3x3 px GpuBoxes in
// the same batch as the detection boxes. Scene->tensor mapping is
// (s, lb_top) -- identical for both loops. Detection boxes win the
// VBO budget (GpuBoxList::cap).

void fill_gpu_boxes( const DetList& dets, const TrajList& trajs, const Tracker& tracker,
                            GpuBoxList& boxes, int model_w, int model_h,
                            float sx, float sy, float lb_left, float lb_top );

