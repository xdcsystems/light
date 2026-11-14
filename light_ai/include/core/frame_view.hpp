// FrameView: what the output stage needs to know about one inferred frame --
// the tensor that holds its pixels and where the frame sits in it.
// Replaces the Slot& that OverlaySink used to take: the output stage no
// longer sees the pool's handoff cell (its mutex, its state, its engine
// pointer), only this read-only description. No SDK dependencies.

#pragma once

#include <cstdint>

#include "core/geometry.hpp"
#include "pod_list.hpp"

// One model-input tensor (a dma-buf, RGB888, model_w x model_h).
struct TensorRef
{
    int fd = -1;            // dma-buf fd (RGA, GPU import, encoder copy)
    void* data = nullptr;   // CPU mapping (CPU box drawing, raw preview)
    uint32_t size = 0;      // bytes, including any row stride
};

// The pool's tensors in slot order: tensor k belongs to pool slot k.
using TensorSet = PodList<TensorRef, 3>;

struct FrameView
{
    int tensor_idx = 0;  // pool slot == tensor index (GPU drawer import cache key)
    TensorRef tensor {};
    LbGeom lb {};        // the frame's letterbox band in the tensor
    int src_idx = 0;     // owning source (multi-view tile)
    int src_w = 0;       // source geometry: scene -> tensor box scale
    int src_h = 0;
    int64_t ts_us = -1;  // pts, us from run() start
};
