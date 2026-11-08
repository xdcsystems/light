#pragma once
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

    typedef EGLImageKHR( EGLAPIENTRYP P_CreateImage )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
    typedef EGLBoolean( EGLAPIENTRYP P_DestroyImage )( EGLDisplay, EGLImageKHR );
    typedef EGLSyncKHR( EGLAPIENTRYP P_CreateSync )( EGLDisplay, EGLenum, const EGLint * );
    typedef EGLBoolean( EGLAPIENTRYP P_DestroySync )( EGLDisplay, EGLSyncKHR );
    typedef EGLBoolean( EGLAPIENTRYP P_ClientWaitSync )( EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR );
    typedef EGLint( EGLAPIENTRYP P_DupNativeFenceFDANDROID )( EGLDisplay, EGLSyncKHR );
    typedef EGLDisplay( EGLAPIENTRYP P_GetPlatformDisplayEXT )( EGLenum, void *, const EGLint * );
    typedef void( GL_APIENTRYP P_ImageTargetTexture2DOES )( GLenum, GLeglImageOES );
    typedef void( GL_APIENTRYP P_GenVertexArrays )( GLsizei, GLuint * );
    typedef void( GL_APIENTRYP P_DeleteVertexArrays )( GLsizei, const GLuint * );
    typedef void( GL_APIENTRYP P_BindVertexArray )( GLuint );

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
        // Size is fixed: 128 boxes (PodList cap) x 24 verts x 12 B = 36864;
        // gpu_box_drawer.cpp pins the layout with a static_assert.
        alignas( 4 ) uint8_t vstage[36864] {};

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
    void make_current();
    bool make_program();
    // Attribute setup for the batched vertex layout: recorded once into
    // the VAO (ES3 path) or re-applied per draw() (GLES2 path).
    void setup_attribs() const;

private:
    State st_ {};
    bool ready_ = false;
};
