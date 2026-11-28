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
#include "core/non_copyable.hpp"
#include "core/status.hpp"

class Log;

// GPU-assisted box & callout drawing straight into an RGB888 dma-buf
// (StreamLayout output canvas on the H.264 path, or NPU input tensor on
// the raw NUT preview path; Mali G610 via EGL/GBM, zero CPU pixels per frame).
// GpuBox / GpuBoxList live in core/gpu_box.hpp.

class GpuBoxDrawer : private NonMovable<GpuBoxDrawer>
{
    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (libmali does not export all of them as symbols).

    using P_CreateImage = EGLImageKHR( EGLAPIENTRYP )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
    using P_DestroyImage = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLImageKHR );
    using P_CreateSync = EGLSyncKHR( EGLAPIENTRYP )( EGLDisplay, EGLenum, const EGLint * );
    using P_DestroySync = EGLBoolean( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR );
    using P_ClientWaitSync = EGLint( EGLAPIENTRYP )( EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR );  // EGL_KHR_fence_sync
    using P_GetPlatformDisplayEXT = EGLDisplay( EGLAPIENTRYP )( EGLenum, void *, const EGLint * );
    using P_ImageTargetTexture2DOES = void( GL_APIENTRYP )( GLenum, GLeglImageOES );
    using P_GenVertexArrays = void( GL_APIENTRYP )( GLsizei, GLuint * );
    using P_DeleteVertexArrays = void( GL_APIENTRYP )( GLsizei, const GLuint * );
    using P_BindVertexArray = void( GL_APIENTRYP )( GLuint );
    // libgbm is dlopen'ed (the mali copy first): its entry points come from dlsym.
    using P_GbmCreateDevice = void *( * )( int );
    using P_GbmDestroyDevice = void ( * )( void * );

    // Batched VBO vertex layout: xy + uv + rgba (20 bytes, align 4).
    // GL red lands in memory byte2 (BGR dma-buf), so RGB888 R goes in .b.
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
        // The label text: the baked outline pass + the fill pass (#108; it was 8
        // offset outline passes + 1 white pass).
        kTextPasses = 2,
        // Connector segments + the longest label (kLabelCap - 1 glyphs) in every
        // text pass, 6 vertices a quad: 6 * (2 + 31 * 2) = 384. Checked against
        // CalloutItem below.
        kVertsPerCallout = 6 * ( 2 + ( glyph_font::kLabelCap - 1 ) * kTextPasses ),
        // One draw is one source's frame: every box of it and one callout per
        // track (CalloutList::cap == TrajList::cap).
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

        // Preallocated draw() staging buffer (128 boxes + 8 callouts):
        // kMaxVerts = 3072 + 8 * 384 = 6144 vertices, 122,880 bytes.
        std::array<BatchVert, kMaxVerts> vstage {};

        // Per-slot import cache. Geometry (w/h/stride) is shared across
        // slots, only the dma-buf fd differs.
        // Slot 0 serves the single RGB888 output canvas (H.264 path) or
        // attach_tensor()/draw(); the raw preview pool uses attach_slot()/draw_slot().
        enum { kSlots = 6 };
        struct SlotImg
        {
            EGLImageKHR img = EGL_NO_IMAGE_KHR;
            GLuint tex = 0, fbo = 0;
        };
        SlotImg slot[kSlots];
        int gbm_fd = -1;  // the GBM device lives on this fd
        void *gbm_dev = nullptr;
        void *dl_gbm = nullptr;
        EGLenum fence_type = 0;

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

    // EGL/GBM display + surfaceless GLES3 context + shader + VBO.
    // On failure everything created so far is released (done()); the cause
    // is in the returned Status, the step details are in the log.
    Status init();
    // Imports an RGB888 dma-buf as an EGLImage-backed FBO, slot-keyed.
    // attach_tensor()/draw() keep the single-target semantics (slot 0);
    // the raw preview pool uses attach_slot()/draw_slot().
    Status attach_slot( int slot, int tensor_dma_fd, int w, int h, uint32_t stride );
    Status attach_tensor( int tensor_dma_fd, int w, int h, uint32_t stride )
    {
        return attach_slot( 0, tensor_dma_fd, w, h, stride );
    }
    // Async GPU draw: 1-px pixel-snapped outlines for all boxes + callouts in ONE draw call
    // (batched VBO, per-vertex color), alpha-blended
    // (Porter-Duff over; the trail fade of BoxMapper is the alpha source).
    // The commands may still sit in the GL queue on return: sync_cpu() submits and waits for them.
    // false: the context could not be made current, nothing was drawn.
    bool draw_slot( int slot, const GpuBox *boxes, int n, const CalloutList *callouts = nullptr ) noexcept;
    bool draw( const GpuBox *boxes, int n, const CalloutList *callouts = nullptr ) noexcept
    {
        return draw_slot( 0, boxes, n, callouts );
    }
    // Blocks until all GPU writes from draw() are visible to CPU/RGA.
    // Fence sync (EGL_KHR_fence_sync -> EGL_ANDROID_native_fence_sync) when available,
    // glFinish() otherwise. Bounded to kFenceWaitNs (100 ms); returns false on fence timeout
    // without calling unbounded glFinish() so a wedged GPU cannot hang the pipeline.
    // false as well when the context cannot be made current.
    bool sync_cpu() noexcept;
    void release_context() const noexcept;
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
    bool check_gl_extensions() const;
    // Uploads the GL_LUMINANCE_ALPHA (fill, outline) glyph + solid-slot atlas texture once during init().
    void create_atlas();
    // The batch VBO, and the VAO on ES3. Cannot fail.
    void create_buffers();
    void destroy_gbm_device( void *dev ) const;

    void done();
    // The program, VBO, VAO and atlas texture. The context must be current.
    void delete_gl_objects();
    // gl_current = false: only the EGLImage is destroyed (no GL calls without a current context).
    void release_slot( int slot, bool gl_current = true );
    // Makes st_.ctx current unless it already is; false (logged) when EGL refuses.
    bool make_current() noexcept;
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

// The per-callout budget covers every connector segment and the longest
// label CalloutItem can hold (kLabelCap - 1 single-byte glyphs) in every text pass.
static_assert( GpuBoxDrawer::kVertsPerCallout >=
                   6 * ( static_cast<int>( sizeof( CalloutItem::segs ) / sizeof( CalloutRect ) ) +
                         ( glyph_font::kLabelCap - 1 ) * GpuBoxDrawer::kTextPasses ),
               "kVertsPerCallout below the longest outlined callout" );
// The whole batch: the box cap plus a full callout for every track a frame can
// carry (8 per source; the up to 4 sources of the 2x2 grid draw separately).
static_assert( static_cast<int>( CalloutList::cap ) >= static_cast<int>( TrajList::cap ), "a frame's track without a callout slot" );
static_assert( static_cast<int>( GpuBoxDrawer::kMaxVerts ) >=
                   static_cast<int>( GpuBoxList::cap ) * static_cast<int>( GpuBoxDrawer::kVertsPerBox ) +
                       static_cast<int>( TrajList::cap ) * static_cast<int>( GpuBoxDrawer::kVertsPerCallout ),
               "vertex budget below the fullest frame" );
