#pragma once
#include <array>
#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// Xlib.h (pulled in via eglplatform.h) defines macros that clash
// with project names. Undefine them right at the X11 entry point.
#pragma push_macro("Status")
#pragma push_macro("None")
#pragma push_macro("Bool")
#pragma push_macro("True")
#pragma push_macro("False")
#pragma push_macro("CursorShape")
#pragma push_macro("RevertToParent")
#undef Status
#undef None
#undef Bool
#undef True
#undef False
#undef CursorShape
#undef RevertToParent

#include "core/callout.hpp"  // CalloutList
#include "core/gpu_box.hpp"  // GpuBox, GpuBoxList
#include "core/status.hpp"

class Log;

// GPU-assisted box drawing straight into the NPU input tensor
// (Mali G610 via EGL/GBM, zero CPU pixels per frame).
// GpuBox / GpuBoxList live in core/gpu_box.hpp.

class GpuBoxDrawer
{
    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (libmali does not export all of them as symbols).

    using P_CreateImage = EGLImageKHR( EGLAPIENTRYP )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
    using P_DestroyImage = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLImageKHR );
    using P_CreateSync = EGLSyncKHR( EGLAPIENTRYP )( EGLDisplay, EGLenum, const EGLint * );
    using P_DestroySync = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR );
    using P_ClientWaitSync = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR );
    using P_GetPlatformDisplayEXT = EGLDisplay( EGLAPIENTRYP )( EGLenum, void *, const EGLint * );
    using P_ImageTargetTexture2DOES = void( GL_APIENTRYP )( GLenum, GLeglImageOES );
    using P_GenVertexArrays = void( GL_APIENTRYP )( GLsizei, GLuint * );
    using P_DeleteVertexArrays = void( GL_APIENTRYP )( GLsizei, const GLuint * );
    using P_BindVertexArray = void( GL_APIENTRYP )( GLuint );
    // libgbm is dlopen'ed (the mali copy first): its entry points come from dlsym.
    using P_GbmCreateDevice = void *( * )( int );
    using P_GbmDestroyDevice = void ( * )( void * );

    // Batched VBO vertex layout: xy + uv + rgba (20 bytes, align 4).
    // GL red lands in memory byte2 (BGR dma-buf), so tensor R goes in .b.
public:
    struct BatchVert
    {
        float x, y;
        float u, v;
        uint8_t r, g, b, a;
    };
    enum
    {
        kVertsPerBox = 4 * 6,
        kVertsPerCallout = 192,
        kMaxVerts = GpuBoxList::cap * kVertsPerBox + CalloutList::cap * kVertsPerCallout
    };

private:
    // GL/EGL state by value: no heap, no pimpl.
    struct State
    {
        EGLDisplay dpy = EGL_NO_DISPLAY;
        EGLContext ctx = nullptr;
        GLuint prog = 0, vbo = 0, vao = 0, atlas_tex = 0;
        GLint apos = -1, auv = -1, acol = -1, utex = -1;
        bool es3 = false;  // ES3 context with VAO entry points
        int w = 0, h = 0;
        uint32_t stride = 0;

        // Preallocated draw() staging buffer (128 boxes + 8 callouts).
        std::array<BatchVert, kMaxVerts> vstage {};

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
        P_GetPlatformDisplayEXT get_platform_display = nullptr;
        P_ImageTargetTexture2DOES image_target_tex = nullptr;
        P_GenVertexArrays gen_vao = nullptr;
        P_DeleteVertexArrays del_vao = nullptr;
        P_BindVertexArray bind_vao = nullptr;
    };

public:
    explicit GpuBoxDrawer( const Log& log ) : log_( log ) {}
    ~GpuBoxDrawer();
    GpuBoxDrawer( const GpuBoxDrawer & ) = delete;
    GpuBoxDrawer &operator=( const GpuBoxDrawer & ) = delete;

    // EGL/GBM display + surfaceless GLES3 context + shader + VBO.
    // On failure everything created so far is released (done()); the cause
    // is in the returned Status, the step details are in the log.
    Status init();
    // Imports the NPU tensor dma-buf as an EGLImage-backed FBO,
    // slot-keyed. attach_tensor()/draw() keep the single-context
    // semantics (slot 0); the pool uses attach_slot()/draw_slot().
    Status attach_slot( int slot, int tensor_dma_fd, int w, int h, uint32_t stride );
    Status attach_tensor( int tensor_dma_fd, int w, int h, uint32_t stride )
    {
        return attach_slot( 0, tensor_dma_fd, w, h, stride );
    }
    // Async GPU draw: 1-px outlines for all boxes in ONE draw call
    // (batched VBO, per-vertex color), alpha-blended
    // (Porter-Duff over; the trail fade of BoxMapper is the alpha source).
    // Switching FBOs between slots is the only added cost vs the single-tensor path.
    void draw_slot( int slot, const GpuBox *boxes, int n, const CalloutList *callouts = nullptr );
    void draw( const GpuBox *boxes, int n, const CalloutList *callouts = nullptr )
    {
        draw_slot( 0, boxes, n, callouts );
    }
    // Blocks until all GPU writes from draw() are visible to CPU/RGA.
    // Native fence (EGL_ANDROID_native_fence_sync) when available, glFinish() otherwise.
    // The fence wait is bounded (100 ms cap) with a glFinish() fallback --
    // a wedged GPU must not hang the pipeline.
    void sync_cpu();
    // Draw -> RGA/CPU ordering is sync_cpu() only: this RGA driver
    // ignores acquire fences, so a fence fd handoff is not a valid
    // mechanism on this board (retired, see docs/state.md).
    bool ok() const { return ready_; }
    uint64_t fence_timeouts() const noexcept { return fence_tmo_cnt_; }

private:
    // The steps of init(), in this order. Each one logs its own failure;
    // the release of a partial init is done() in init() alone.
    // GBM display: dlopen libgbm, then the first DRM node that initializes.
    bool open_display();
    // One DRM node: fd + GBM device + EGL display; a failed candidate releases what it created.
    bool try_node( const char *path, P_GbmCreateDevice create_dev );
    // The image import extensions and the extension entry points.
    bool load_entry_points();
    // Config (with the fallback), ES3 context then ES2, made current.
    bool create_context();
    // Uploads GL_ALPHA glyph + solid-slot atlas texture once during init().
    void create_atlas();
    // The batch VBO, and the VAO on ES3. Cannot fail.
    void create_buffers();
    void destroy_gbm_device( void *dev ) const;

    void done();
    void release_slot( int slot );
    void make_current();
    bool make_program();
    // Attribute setup for the batched vertex layout: recorded once into
    // the VAO (ES3 path) or re-applied per draw() (GLES2 path).
    void setup_attribs() const;

private:
    const Log& log_;
    State st_ {};
    bool ready_ = false;
    uint64_t fence_tmo_cnt_ = 0;
};
