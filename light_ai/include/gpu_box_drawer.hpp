#pragma once
#include <array>
#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "pod_list.hpp"

// GPU-assisted box drawing straight into the NPU input tensor
// (Mali G610 via EGL/GBM, zero CPU pixels per frame).
// Colors are in tensor RGB888 semantics (byte0 = R);
// the implementation swaps R/B internally -- the dma-buf reads as BGR to the GPU.
struct GpuBox
{
    float x0, y0, x1, y1;  // tensor pixels, top-left origin
    uint8_t r, g, b, a;
};

using GpuBoxList = PodList<GpuBox, 128>;

class GpuBoxDrawer
{
    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (libmali does not export all of them as symbols).

    using P_CreateImage = EGLImageKHR( EGLAPIENTRYP )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
    using P_DestroyImage = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLImageKHR );
    using P_CreateSync = EGLSyncKHR( EGLAPIENTRYP )( EGLDisplay, EGLenum, const EGLint * );
    using P_DestroySync = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR );
    using P_ClientWaitSync = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR );
    using P_DupNativeFenceFDANDROID = EGLint( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR );
    using P_GetPlatformDisplayEXT = EGLDisplay( EGLAPIENTRYP )( EGLenum, void *, const EGLint * );
    using P_ImageTargetTexture2DOES = void( GL_APIENTRYP )( GLenum, GLeglImageOES );
    using P_GenVertexArrays = void( GL_APIENTRYP )( GLsizei, GLuint * );
    using P_DeleteVertexArrays = void( GL_APIENTRYP )( GLsizei, const GLuint * );
    using P_BindVertexArray = void( GL_APIENTRYP )( GLuint );

    // Batched VBO vertex layout (moved out of gpu_box_drawer.cpp: the
    // draw() staging buffer is typed by it). r/g/b/a fields are GL
    // component order; GL red lands in memory byte2 (the dma-buf reads
    // as BGR -- probe calibration), so the TENSOR R is stored in the
    // field named 'b'. 12 bytes, natural align 4: no padding, no alignas.
public:
    struct BatchVert
    {
        float x, y;
        uint8_t r, g, b, a;
    };
    static constexpr int kVertsPerBox = 4 * 6;  // 4 quads x 6 vertices per box

private:
    // GL/EGL state by value: no heap, no pimpl.
    struct State
    {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLContext ctx = nullptr;
        GLuint prog = 0, vbo = 0, vao = 0;
        GLint apos = -1, acol = -1;
        bool es3 = false;  // ES3 context with VAO entry points
        int w = 0, h = 0;
        uint32_t stride = 0;

        // Preallocated draw() staging buffer, not a per-call stack array.
        // Typed: one slot per vertex of the batched VBO
        // (GpuBoxList cap x kVertsPerBox; see BatchVert above).
        std::array<BatchVert, GpuBoxList::cap * kVertsPerBox> vstage {};

        // Per-slot import cache. Geometry (w/h/stride) is shared across
        // slots, only the dma-buf fd differs.
        // Slot 0 serves attach_tensor()/draw() -- the single-context path;
        // the pool uses attach_slot()/draw_slot().
        enum { kSlots = 3 };
        struct SlotImg
        {
            EGLImageKHR img = EGL_NO_IMAGE_KHR;
            GLuint tex = 0, fbo = 0;
        };
        SlotImg slot[kSlots];
        int gbm_fd = -1;  // the GBM device lives on this fd
        void *gbm_dev = nullptr;
        void *dl_gbm = nullptr;
        bool native_fence = false;

        P_CreateImage create_image = nullptr;
        P_DestroyImage destroy_image = nullptr;
        P_CreateSync create_sync = nullptr;
        P_DestroySync destroy_sync = nullptr;
        P_ClientWaitSync client_wait = nullptr;
        P_DupNativeFenceFDANDROID dup_fence = nullptr;  // RGA handoff
        P_GetPlatformDisplayEXT get_platform_display = nullptr;
        P_ImageTargetTexture2DOES image_target_tex = nullptr;
        P_GenVertexArrays gen_vao = nullptr;
        P_DeleteVertexArrays del_vao = nullptr;
        P_BindVertexArray bind_vao = nullptr;
    };

public:
    GpuBoxDrawer();
    ~GpuBoxDrawer();
    GpuBoxDrawer( const GpuBoxDrawer & ) = delete;
    GpuBoxDrawer &operator=( const GpuBoxDrawer & ) = delete;

    // EGL/GBM display + surfaceless GLES3 context + shader + VBO.
    bool init();
    // Imports the NPU tensor dma-buf as an EGLImage-backed FBO,
    // slot-keyed. attach_tensor()/draw() keep the single-context
    // semantics (slot 0); the pool uses attach_slot()/draw_slot().
    bool attach_slot( int slot, int tensor_dma_fd, int w, int h, uint32_t stride );
    bool attach_tensor( int tensor_dma_fd, int w, int h, uint32_t stride )
    {
        return attach_slot( 0, tensor_dma_fd, w, h, stride );
    }
    // Async GPU draw: 1-px outlines for all boxes in ONE draw call
    // (batched VBO, per-vertex color), alpha-blended
    // (Porter-Duff over; the trail fade in fill_gpu_boxes is the alpha source).
    // Switching FBOs between slots is the only added cost vs the single-tensor path.
    void draw_slot( int slot, const GpuBox *boxes, int n );
    void draw( const GpuBox *boxes, int n )
    {
        draw_slot( 0, boxes, n );
    }
    // Blocks until all GPU writes from draw() are visible to CPU/RGA.
    // Native fence (EGL_ANDROID_native_fence_sync) when available, glFinish() otherwise.
    // The fence wait is bounded (100 ms cap) with a glFinish() fallback --
    // a wedged GPU must not hang the pipeline.
    void sync_cpu();
    // Export a native fence fd covering the writes of the last draw().
    // Ownership passes to the caller, which must close() it
    // (or wait on it, e.g. poll() in the encoder before the RGA convert; then close() it).
    // NOTE: this RGA driver ignores acquire fences,
    // so fd handoff to improcess is NOT a valid use.
    // Returns -1 when the native fence extension is unavailable --
    // the caller must fall back to sync_cpu().
    int export_fence();
    bool ok() const { return ready_; }

private:
    void done();
    void release_slot( int slot );
    void make_current();
    bool make_program();
    // Attribute setup for the batched vertex layout: recorded once into
    // the VAO (ES3 path) or re-applied per draw() (GLES2 path).
    void setup_attribs() const;

private:
    State st_ {};
    bool ready_ = false;
};
