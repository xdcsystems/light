#pragma once
#include <cstdint>
#include <vector>

// M3.7: GPU-assisted box drawing straight into the NPU input tensor
// (Mali G610 via EGL/GBM, zero CPU pixels per frame).
// Colors are in tensor RGB888 semantics (byte0 = R); the implementation
// swaps R/B internally — the dma-buf reads as BGR to the GPU (calibrated
// by tools/egl_probe.cpp).
struct GpuBox {
    float x0, y0, x1, y1;   // tensor pixels, top-left origin
    uint8_t r, g, b, a;
};

class GpuBoxDrawer {
public:
    GpuBoxDrawer();
    ~GpuBoxDrawer();
    GpuBoxDrawer(const GpuBoxDrawer&) = delete;
    GpuBoxDrawer& operator=(const GpuBoxDrawer&) = delete;

    // EGL/GBM display + surfaceless GLES3 context + shader + VBO.
    bool init();
    // Imports the NPU tensor dma-buf as an EGLImage-backed FBO.
    bool attachTensor(int tensorDmaFd, int w, int h, uint32_t stride);
    // Async GPU draw: one 1-px outline per box.
    void draw(const std::vector<GpuBox>& boxes);
    // Blocks until all GPU writes from draw() are visible to CPU/RGA.
    // Native fence (EGL_ANDROID_native_fence_sync) when available,
    // glFinish() otherwise.
    void syncCpu();
    bool ok() const { return ready_; }

private:
    void done();
    void* st_;          // pimpl: EGL/GL state, hidden from this header
    bool ready_ = false;
};
