// GpuBox: one box of the overlay batch in tensor coordinates. Shared by the
// box mapper (producer) and both draw paths (GpuBoxDrawer, draw_boxes_cpu).
// No EGL/GLES dependencies (moved out of gpu_box_drawer.hpp so the mapper
// stops including the GL layer).

#pragma once

#include <cstdint>

#include "pod_list.hpp"

// Colors are in tensor RGB888 semantics (byte0 = R);
// the GPU implementation swaps R/B internally -- the dma-buf reads as BGR to the GPU.
struct GpuBox
{
    float x0, y0, x1, y1;  // tensor pixels, top-left origin
    uint8_t r, g, b, a;
};

using GpuBoxList = PodList<GpuBox, 128>;
