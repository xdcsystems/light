#pragma once
#include <cstdint>

#include "pod_list.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// M3.7: GPU-assisted box drawing straight into the NPU input tensor
// (Mali G610 via EGL/GBM, zero CPU pixels per frame).
// Colors are in tensor RGB888 semantics (byte0 = R); the implementation
// swaps R/B internally — the dma-buf reads as BGR to the GPU (calibrated
// by tools/egl_probe.cpp).
struct GpuBox
{
    float x0, y0, x1, y1;  // tensor pixels, top-left origin
    uint8_t r, g, b, a;
};

using GpuBoxList = PodList<GpuBox, 128>;

class GpuBoxDrawer
{
public:
    GpuBoxDrawer();
    ~GpuBoxDrawer();
    GpuBoxDrawer( const GpuBoxDrawer & ) = delete;
    GpuBoxDrawer &operator=( const GpuBoxDrawer & ) = delete;

    // EGL/GBM display + surfaceless GLES3 context + shader + VBO.
    bool init();
    // Imports the NPU tensor dma-buf as an EGLImage-backed FBO.
    bool attach_tensor( int tensor_dma_fd, int w, int h, uint32_t stride );
    // Async GPU draw: 1-px outlines for all boxes in ONE draw call
    // (batched VBO, per-vertex color).
    void draw( const GpuBox *boxes, int n );
    // Blocks until all GPU writes from draw() are visible to CPU/RGA.
    // Native fence (EGL_ANDROID_native_fence_sync) when available,
    // glFinish() otherwise. The fence wait is bounded (100 ms cap) with
    // a glFinish() fallback — a wedged GPU must not hang the pipeline.
    void sync_cpu();
    // Export a native fence fd covering the writes of the last draw().
    // Ownership passes to the caller, which must close() it (or wait on
    // it, e.g. poll() in the encoder before the RGA convert; then
    // close() it). NOTE: this RGA driver ignores acquire fences, so
    // fd handoff to improcess is NOT a valid use.
    // Returns -1 when the native fence extension is unavailable — the
    // caller must fall back to sync_cpu().
    int export_fence();
    bool ok() const { return ready_; }

private:
    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (the probe proved libmali does
    // not export all of them as symbols).
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

    // GL/EGL state by value: no heap, no pimpl. The old State* was the
    // only raw owning pointer in the production code (audit 2026-10-27).
    struct State
    {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLContext ctx = nullptr;
        EGLImageKHR img = EGL_NO_IMAGE_KHR;
        GLuint tex = 0, fbo = 0, prog = 0, vbo = 0, vao = 0;
        GLint apos = -1, acol = -1;
        bool es3 = false;  // ES3 context with VAO entry points
        int w = 0, h = 0;
        uint32_t stride = 0;
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

    void done();
    void make_current();
    bool make_program();
    // Attribute setup for the batched vertex layout: recorded once into
    // the VAO (ES3 path) or re-applied per draw() (GLES2 path).
    void setup_attribs() const;

private:  // data
    State st_ {};
    bool ready_ = false;
};
