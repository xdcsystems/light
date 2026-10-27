/* M3.7: GPU box drawing (Mali G610) straight into the NPU input tensor.
 * Calibration facts from tools/egl_probe.cpp (RK3588, libmali -x11-gbm):
 *   - EGL platform: GBM (mali libgbm first, then the system libgbm).
 *   - dma-buf RGB888 import OK; 24bpp dumb pitch == w*3 (no alignment).
 *   - GL writes red into memory byte2, i.e. it sees the buffer as BGR:
 *     the desired tensor R goes into the GL BLUE uniform channel.
 *   - EGL_ANDROID_native_fence_sync is available and usable.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <initializer_list>
#include <iostream>
#include <linux/dma-buf.h>
#include <cstring>
#include <unistd.h>
#include <string_view>

#include "gpu_box_drawer.hpp"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFull
#endif
#ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#endif

// 100 ms cap for sync_cpu(): a wedged GPU must not hang the pipeline.
constexpr EGLTimeKHR kFenceWaitNs = 100ull * 1000 * 1000;

namespace
{
    bool has( const char *exts, const char *name ) { return exts && strstr( exts, name ) != nullptr; }

    // Per-vertex color (normalized ubyte attribute) instead of the per-box
    // uniform: that is what allows all boxes to be batched into ONE draw
    // call. Channel order is GL semantics — see the r/b swap note in draw().
    constexpr const char *kVS =
            "attribute vec2 a_pos; attribute vec4 a_col; varying vec4 v_col; "
            "void main(){ gl_Position=vec4(a_pos,0.,1.); v_col=a_col; }";
    constexpr const char *kFS =
            "precision mediump float; varying vec4 v_col; "
            "void main(){ gl_FragColor=v_col; }";

    // Batched vertex: 2 floats + 4 normalized ubytes = 12 bytes. The
    // r/g/b/a fields are GL component order; GL red lands in memory byte2
    // (the dma-buf reads as BGR — probe calibration), so the TENSOR R is
    // stored in the field named 'b'.
    struct BatchVert
    {
        float x, y;
        uint8_t r, g, b, a;
    };

    // PodList<GpuBox, 128> hard cap (mirrors the header type). One VBO for
    // the whole session: 4 quads x 6 vertices per box, allocated once in
    // init(); draw() only uploads — zero reallocations.
    constexpr int kMaxBoxes = 128;
    constexpr int kVertsPerBox = 4 * 6;
    constexpr GLsizeiptr kVboBytes = kMaxBoxes * kVertsPerBox * (GLsizeiptr)sizeof( BatchVert );

    // Compile/link diagnostics: libmali returns non-zero handles even for
    // failed shaders — without these checks a broken program degrades
    // silently into "nothing is drawn".
    bool shader_ok( GLuint sh, const char *what )
    {
        GLint ok = 0;
        glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
        if ( ok )
            return true;
        char log[512];
        GLsizei n = 0;
        glGetShaderInfoLog( sh, sizeof( log ), &n, log );
        log[n] = 0;
        std::cerr << "gpu_draw: " << what << " shader compile failed:\n" << log << '\n';
        return false;
    }

}  // anonymous namespace

void GpuBoxDrawer::make_current()  // NOLINT(readability-make-member-function-const)
{
    // Same-context eglMakeCurrent is not free in libmali; skip it when
    // the context is already current (the common case in the hot path).
    if ( eglGetCurrentContext() != st_.ctx )
        eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st_.ctx );
}

bool GpuBoxDrawer::make_program()
{
    GLuint vs = glCreateShader( GL_VERTEX_SHADER );
    glShaderSource( vs, 1, &kVS, nullptr );
    glCompileShader( vs );
    GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
    glShaderSource( fs, 1, &kFS, nullptr );
    glCompileShader( fs );

    if ( !shader_ok( vs, "vertex" ) || !shader_ok( fs, "fragment" ) )
    {
        glDeleteShader( vs );
        glDeleteShader( fs );
        return false;
    }

    st_.prog = glCreateProgram();
    glAttachShader( st_.prog, vs );
    glAttachShader( st_.prog, fs );
    glLinkProgram( st_.prog );

    GLint linked = 0;
    glGetProgramiv( st_.prog, GL_LINK_STATUS, &linked );
    if ( !linked )
    {
        char log[512];
        GLsizei n = 0;
        glGetProgramInfoLog( st_.prog, sizeof( log ), &n, log );
        std::cerr << "gpu_draw: program link failed:\n" << std::string_view( log, n ) << '\n';
        glDeleteProgram( st_.prog );
        st_.prog = 0;
        glDeleteShader( vs );
        glDeleteShader( fs );
        return false;
    }
    glDeleteShader( vs );  // прилинкованы — ссылки шейдеров больше не нужны
    glDeleteShader( fs );

    st_.apos = glGetAttribLocation( st_.prog, "a_pos" );
    st_.acol = glGetAttribLocation( st_.prog, "a_col" );

    if ( st_.apos < 0 || st_.acol < 0 )
    {
        std::cerr << "gpu_draw: a_pos/a_col missing from linked program\n";
        glDeleteProgram( st_.prog );
        st_.prog = 0;
        return false;
    }

    return st_.prog != 0;
}

// Attribute setup for the batched vertex layout (12-byte stride). The
// color field offset is fixed: 2 x sizeof(float) = 8 bytes.
void GpuBoxDrawer::setup_attribs() const
{
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glEnableVertexAttribArray( st_.apos );
    glVertexAttribPointer( st_.apos, 2, GL_FLOAT, GL_FALSE, sizeof( BatchVert ), (void *)nullptr );
    if ( st_.acol >= 0 )
    {
        glEnableVertexAttribArray( st_.acol );
        glVertexAttribPointer( st_.acol, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( BatchVert ), (void *)8 );
    }
}

GpuBoxDrawer::GpuBoxDrawer()
= default;

GpuBoxDrawer::~GpuBoxDrawer()
{
    done();
}

bool GpuBoxDrawer::init()
{
    done();
    st_ = State {};
    ready_ = false;

    st_.get_platform_display = (P_GetPlatformDisplayEXT)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
    if ( !st_.get_platform_display )
    {
        std::cerr << "drawer: eglGetPlatformDisplayEXT missing\n";
        return false;
    }
    // GBM display: the mali libgbm first, then the system one
    // (calibrated in the probe: mali's copy initializes cleanly).
    st_.dl_gbm = dlopen( "/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !st_.dl_gbm )
        st_.dl_gbm = dlopen( "libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !st_.dl_gbm )
    {
        std::cerr << "drawer: libgbm not loadable\n";
        return false;
    }
    typedef void *( *P_gbm_create_device )( int );
    P_gbm_create_device gbm_create_device = (P_gbm_create_device)dlsym( st_.dl_gbm, "gbm_create_device" );
    if ( !gbm_create_device )
    {
        std::cerr << "drawer: gbm_create_device missing\n";
        return false;
    }
    for ( const char *node : { "/dev/dri/renderD128", "/dev/dri/card0" } )
    {
        int fd = open( node, O_RDWR | O_CLOEXEC );
        if ( fd < 0 )
            continue;
        void *dev = gbm_create_device( fd );
        if ( !dev )
        {
            close( fd );
            continue;
        }
        EGLDisplay d = st_.get_platform_display( EGL_PLATFORM_GBM_KHR, dev, nullptr );
        EGLint maj = 0;
        EGLint min = 0;
        if ( d == EGL_NO_DISPLAY || !eglInitialize( d, &maj, &min ) )
        {
            // failed init candidate: release what this attempt created
            // (EGL display + GBM device + fd). The dl_gbm handle stays:
            // gbm_create_device may be needed by the next node attempt,
            // done() closes it on the terminal failure path.
            if ( d != EGL_NO_DISPLAY )
                eglTerminate( d );
            if ( dev )
            {
                typedef void ( *P_gbm_destroy_device )( void * );
                auto destroy_dev = (P_gbm_destroy_device)dlsym( st_.dl_gbm, "gbm_destroy_device" );
                if ( destroy_dev )
                    destroy_dev( dev );
            }
            close( fd );
            continue;
        }

        st_.dpy = d;
        st_.gbm_fd = fd;  // kept open: gbm_dev is bound to it
        st_.gbm_dev = dev;
        break;
    }
    if ( st_.dpy == EGL_NO_DISPLAY )
    {
        std::cerr << "drawer: EGL/GBM init failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }

    const char *exts = eglQueryString( st_.dpy, EGL_EXTENSIONS );
    if ( !has( exts, "EGL_EXT_image_dma_buf_import" ) || !has( exts, "EGL_KHR_image_base" ) )
    {
        std::cerr << "drawer: no image import extensions\n";
        return false;
    }

    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (the probe proved libmali does
    // not export all of them as symbols).
    st_.create_image = (P_CreateImage)eglGetProcAddress( "eglCreateImageKHR" );
    st_.destroy_image = (P_DestroyImage)eglGetProcAddress( "eglDestroyImageKHR" );
    st_.create_sync = (P_CreateSync)eglGetProcAddress( "eglCreateSyncKHR" );
    st_.destroy_sync = (P_DestroySync)eglGetProcAddress( "eglDestroySyncKHR" );
    st_.client_wait = (P_ClientWaitSync)eglGetProcAddress( "eglClientWaitSyncKHR" );
    st_.dup_fence = (P_DupNativeFenceFDANDROID)eglGetProcAddress( "eglDupNativeFenceFDANDROID" );
    st_.image_target_tex = (P_ImageTargetTexture2DOES)eglGetProcAddress( "glEGLImageTargetTexture2DOES" );
    st_.gen_vao = (P_GenVertexArrays)eglGetProcAddress( "glGenVertexArrays" );
    st_.del_vao = (P_DeleteVertexArrays)eglGetProcAddress( "glDeleteVertexArrays" );
    st_.bind_vao = (P_BindVertexArray)eglGetProcAddress( "glBindVertexArray" );

    if ( !st_.create_image || !st_.image_target_tex )
    {
        std::cerr << "drawer: image entry points missing\n";
        return false;
    }
    st_.native_fence = has( exts, "EGL_ANDROID_native_fence_sync" ) && st_.create_sync && st_.client_wait;

    EGLConfig cfg = nullptr;
    EGLint n = 0;
    const EGLint cattr[] = { EGL_SURFACE_TYPE,
            EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE,
            EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,
            8,
            EGL_GREEN_SIZE,
            8,
            EGL_BLUE_SIZE,
            8,
            EGL_NONE };
    if ( !eglChooseConfig( st_.dpy, cattr, &cfg, 1, &n ) || n < 1 )
    {
        const EGLint cattr2[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        eglChooseConfig( st_.dpy, cattr2, &cfg, 1, &n );
    }
    const EGLint c3[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    const EGLint c2[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE };
    st_.ctx = eglCreateContext( st_.dpy, cfg, EGL_NO_CONTEXT, c3 );
    if ( !st_.ctx )
        st_.ctx = eglCreateContext( st_.dpy, cfg, EGL_NO_CONTEXT, c2 );
    if ( !st_.ctx || !eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st_.ctx ) )
    {
        std::cerr << "drawer: context failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    if ( !make_program() )
    {
        std::cerr << "drawer: program failed\n";
        return false;
    }
    glGenBuffers( 1, &st_.vbo );
    // Pre-allocate the batch VBO once: draw() only uploads into it.
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glBufferData( GL_ARRAY_BUFFER, kVboBytes, nullptr, GL_STREAM_DRAW );
    // VAO (ES3 only, when the entry points exist): attributes are
    // recorded once here; draw() then does a single glBindVertexArray.
    const char *glver = (const char *)glGetString( GL_VERSION );
    st_.es3 = glver && strstr( glver, "ES 3." ) != nullptr && st_.gen_vao && st_.del_vao && st_.bind_vao;
    if ( st_.es3 )
    {
        st_.gen_vao( 1, &st_.vao );
        st_.bind_vao( st_.vao );
        setup_attribs();
        st_.bind_vao( 0 );
        std::cout << "drawer: batch VAO ready (ES3)\n";
    }
    ready_ = true;
    return true;
}

bool GpuBoxDrawer::attach_tensor( int tensor_dma_fd, int w, int h, uint32_t stride )
{
    if ( !ready_ )
        return false;
    make_current();

    EGLint iattr[] = { EGL_WIDTH,
            w,
            EGL_HEIGHT,
            h,
            EGL_LINUX_DRM_FOURCC_EXT,
            /* DRM_FORMAT_RGB888 */ 0x34324752,
            EGL_DMA_BUF_PLANE0_FD_EXT,
            tensor_dma_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            (EGLint)stride,
            EGL_NONE };
    if ( st_.img != EGL_NO_IMAGE_KHR && st_.destroy_image )
        st_.destroy_image( st_.dpy, st_.img );
    st_.img = st_.create_image( st_.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, ( EGLClientBuffer ) nullptr, iattr );
    if ( st_.img == EGL_NO_IMAGE_KHR )
    {
        std::cerr << "drawer: import tensor failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    if ( st_.tex )
        glDeleteTextures( 1, &st_.tex );
    glGenTextures( 1, &st_.tex );
    glBindTexture( GL_TEXTURE_2D, st_.tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    st_.image_target_tex( GL_TEXTURE_2D, (GLeglImageOES)st_.img );
    if ( st_.fbo )
        glDeleteFramebuffers( 1, &st_.fbo );
    glGenFramebuffers( 1, &st_.fbo );
    glBindFramebuffer( GL_FRAMEBUFFER, st_.fbo );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st_.tex, 0 );
    const GLenum fst = glCheckFramebufferStatus( GL_FRAMEBUFFER );
    if ( fst != GL_FRAMEBUFFER_COMPLETE )
    {
        std::cerr << "drawer: FBO incomplete (status 0x" << std::hex << fst << std::dec << ")\n";
        return false;
    }
    st_.w = w;
    st_.h = h;
    st_.stride = stride;
    std::cout << "drawer: tensor attached " << w << 'x' << h << " stride=" << stride
              << " (native fence: " << ( st_.native_fence ? "yes" : "no" ) << ")\n";
    return true;
}

void GpuBoxDrawer::draw( const GpuBox *boxes, int n )
{
    if ( !ready_ || boxes == nullptr || n <= 0 )
        return;
    if ( n > kMaxBoxes )
        n = kMaxBoxes;
    make_current();

    // Batched quads in tensor coordinates. The viewport clips anything
    // outside the tensor, matching the CPU-path clamping.
    // Y orientation: memory row r maps to NDC y = -1 + 2*r/h — GL's origin
    // (bottom-left) IS memory row 0, so there is NO flip. The probe's
    // green-band test sat at the center row, which is symmetric under a
    // flip and could not catch this; a live frame did (boxes were mirrored
    // around y = h/2).
    BatchVert v[kMaxBoxes * kVertsPerBox];  // ~36 KB on stack
    int cnt = 0;
    const float xs = 2.0f / (float)st_.w;
    const float ys = 2.0f / (float)st_.h;
    const float t = 1.0f;  // outline thickness, tensor px

    auto push = [&]( float x0, float y0, float x1, float y1, const GpuBox &b ) {
        if ( x1 - x0 < 1.0f || y1 - y0 < 1.0f )
            return;
        if ( cnt + 6 > kMaxBoxes * kVertsPerBox )
            return;

        const float q[6][2] = {
                { x0 * xs - 1.0f, y0 * ys - 1.0f },
                { x1 * xs - 1.0f, y0 * ys - 1.0f },
                { x0 * xs - 1.0f, y1 * ys - 1.0f },
                { x1 * xs - 1.0f, y0 * ys - 1.0f },
                { x1 * xs - 1.0f, y1 * ys - 1.0f },
                { x0 * xs - 1.0f, y1 * ys - 1.0f },
        };
        // The dma-buf reads as BGR to the GPU: the tensor R must go into
        // the GL blue channel (probe calibration) — see BatchVert.
        for ( const auto &p : q )
        {
            v[cnt].x = p[0];
            v[cnt].y = p[1];
            v[cnt].r = b.b;
            v[cnt].g = b.g;
            v[cnt].b = b.r;
            v[cnt].a = b.a;
            ++cnt;
        }
    };
    for ( int i = 0; i < n; ++i )
    {
        const GpuBox &b = boxes[i];
        push( b.x0, b.y0, b.x1, b.y0 + t, b );  // top edge
        push( b.x0, b.y1 - t, b.x1, b.y1, b );  // bottom edge
        push( b.x0, b.y0, b.x0 + t, b.y1, b );  // left edge
        push( b.x1 - t, b.y0, b.x1, b.y1, b );  // right edge
    }
    if ( cnt == 0 )
        return;

    if ( st_.es3 )
        st_.bind_vao( st_.vao );
    glBindFramebuffer( GL_FRAMEBUFFER, st_.fbo );
    glViewport( 0, 0, st_.w, st_.h );
    glUseProgram( st_.prog );
    glDisable( GL_DEPTH_TEST );
    glDisable( GL_BLEND );
    if ( !st_.es3 )
        setup_attribs();  // ES3: attributes live in the VAO
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr)cnt * sizeof( BatchVert ), v );
    glDrawArrays( GL_TRIANGLES, 0, cnt );
}

void GpuBoxDrawer::sync_cpu()
{
    if ( st_.dpy == EGL_NO_DISPLAY )
        return;
    if ( st_.native_fence )
    {
        make_current();
        EGLSyncKHR sync = st_.create_sync( st_.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr );
        if ( sync != EGL_NO_SYNC_KHR )
        {
            // Flush bit: without it the fence may never signal if the
            // driver has not flushed the command stream. Bounded wait:
            // a wedged GPU must not hang the pipeline.
            const EGLint r = st_.client_wait( st_.dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, kFenceWaitNs );
            if ( st_.destroy_sync )
                st_.destroy_sync( st_.dpy, sync );
            if ( r == EGL_CONDITION_SATISFIED_KHR )
                return;
            std::cerr << "drawer: fence wait timed out, glFinish fallback\n";
        }
    }
    glFinish();
}

int GpuBoxDrawer::export_fence()
{
    if ( st_.dpy == EGL_NO_DISPLAY )
        return -1;
    if ( !st_.native_fence || !st_.dup_fence )
        return -1;
    make_current();
    EGLSyncKHR sync = st_.create_sync( st_.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr );
    if ( sync == EGL_NO_SYNC_KHR )
        return -1;
    // dup'ed fd: our copy, we own it — the caller must close() it (or
    // hand it to an RGA acquire, which consumes it the same way).
    const int fd = st_.dup_fence( st_.dpy, sync );
    st_.destroy_sync( st_.dpy, sync );
    return fd;
}

void GpuBoxDrawer::done()
{
    if ( st_.dpy != EGL_NO_DISPLAY )
    {
        make_current();
        if ( st_.vbo )
            glDeleteBuffers( 1, &st_.vbo );
        st_.vbo = 0;
        if ( st_.prog )
            glDeleteProgram( st_.prog );
        st_.prog = 0;
        if ( st_.fbo )
            glDeleteFramebuffers( 1, &st_.fbo );
        if ( st_.tex )
            glDeleteTextures( 1, &st_.tex );
        if ( st_.vao && st_.del_vao )
            st_.del_vao( 1, &st_.vao );
        st_.vao = 0;
        if ( st_.img != EGL_NO_IMAGE_KHR && st_.destroy_image )
            st_.destroy_image( st_.dpy, st_.img );
        st_.img = EGL_NO_IMAGE_KHR;
        st_.fbo = 0;
        st_.tex = 0;
        if ( st_.ctx )
        {
            eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
            eglDestroyContext( st_.dpy, st_.ctx );
            st_.ctx = nullptr;
        }
        eglTerminate( st_.dpy );
        st_.dpy = EGL_NO_DISPLAY;
    }
    // GBM device outlives EGL on its fd: destroy it first, then close
    // the fd, then drop the dlopen handle. All three were leaked by the
    // old code on every done() (audit 2026-10-27). Unconditional: init
    // may fail after dlopen/dlopen+open, and those handles must go too.
    if ( st_.dl_gbm )
    {
        typedef void ( *P_gbm_destroy_device )( void * );
        if ( st_.gbm_dev )
        {
            auto destroy_dev = (P_gbm_destroy_device)dlsym( st_.dl_gbm, "gbm_destroy_device" );
            if ( destroy_dev )
                destroy_dev( st_.gbm_dev );
            st_.gbm_dev = nullptr;
        }
        dlclose( st_.dl_gbm );
        st_.dl_gbm = nullptr;
    }
    if ( st_.gbm_fd >= 0 )
    {
        close( st_.gbm_fd );
        st_.gbm_fd = -1;
    }
    ready_ = false;
}
