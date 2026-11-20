// The CPU overlay raster: box outlines and track label callouts drawn straight
// into an RGB888 image (the NPU input tensor, through its CPU mapping). The
// fallback of the GPU drawer (GpuBoxDrawer) when it is unavailable, or forced by
// --cpu-draw (A/B baseline): the same geometry, colors and blending as the GPU
// path. SDK-free, host-tested (tests/test_overlay_raster.cpp); the DMA cache
// sync of the image is the caller's (OverlaySink).
//
// rgb: w x h pixels, 3 bytes each (byte0 = R), rows `stride` bytes apart.
// Nothing is written outside [0, w) x [0, h).

#pragma once

#include <cstdint>

#include "core/callout.hpp"  // CalloutList
#include "core/gpu_box.hpp"  // GpuBox

// 1-px outlines of boxes[0..n), in tensor coordinates (the GPU path's geometry):
// fully off-screen and degenerate (< 1 px) boxes are skipped, the pixel span uses
// floor / ceil like the rasterizer, and the per-box alpha blends over the image
// (Porter-Duff over; a == 255 is an opaque write).
void draw_boxes_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox* boxes, int n ) noexcept;

// Every callout of list: the connector, then the outlined white text (the GPU
// path's order and colors).
void draw_callouts_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutList& list ) noexcept;
