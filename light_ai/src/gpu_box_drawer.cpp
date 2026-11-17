/* GPU box drawing (Mali G610) straight into the NPU input tensor.
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
#include <algorithm>
#include <cstddef>
#include <dlfcn.h>
#include <fcntl.h>
#include <initializer_list>
#include <linux/dma-buf.h>
#include <cstring>
#include <unistd.h>
#include <string_view>
#include <drm/drm_fourcc.h>

#include "gpu_box_drawer.hpp"
#include "core/glyph_font.hpp"
#include "core/log.hpp"

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
            "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col; "
            "varying vec2 v_uv; varying vec4 v_col; "
            "void main(){ gl_Position=vec4(a_pos,0.,1.); v_uv=a_uv; v_col=a_col; }";
    constexpr const char *kFS =
            "precision mediump float; uniform sampler2D u_tex; "
            "varying vec2 v_uv; varying vec4 v_col; "
            "void main(){ gl_FragColor=vec4(v_col.rgb, v_col.a * texture2D(u_tex, v_uv).a); }";

    // One VBO for boxes (128 * 24) + callouts (8 * 192), allocated once in init().
    constexpr GLsizeiptr kVboBytes =
            static_cast<GLsizeiptr>( GpuBoxDrawer::kMaxVerts ) *
            static_cast<GLsizeiptr>( sizeof( GpuBoxDrawer::BatchVert ) );

    struct QuadTarget
    {
        GpuBoxDrawer::BatchVert *v;
        int cnt;
        float xs;
        float ys;
        float solid_u;
        float solid_v;
    };

    void emit_quad( QuadTarget& qt,
                    float x0, float y0, float x1, float y1,
                    float u0, float v0, float u1, float v1,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a ) noexcept
    {
        if ( x1 <= x0 || y1 <= y0 || qt.cnt + 6 > GpuBoxDrawer::kMaxVerts )
            return;
        const float nx0 = x0 * qt.xs - 1.0f;
        const float ny0 = y0 * qt.ys - 1.0f;
        const float nx1 = x1 * qt.xs - 1.0f;
        const float ny1 = y1 * qt.ys - 1.0f;
        const float pos[6][4] = {
            { nx0, ny0, u0, v0 },
            { nx1, ny0, u1, v0 },
            { nx0, ny1, u0, v1 },
            { nx1, ny0, u1, v0 },
            { nx1, ny1, u1, v1 },
            { nx0, ny1, u0, v1 },
        };
        // BGR dma-buf swap: tensor R lands in GL blue channel.
        for ( const auto& p : pos )
        {
            GpuBoxDrawer::BatchVert& dst = qt.v[qt.cnt++];
            dst.x = p[0];
            dst.y = p[1];
            dst.u = p[2];
            dst.v = p[3];
            dst.r = b;
            dst.g = g;
            dst.b = r;
            dst.a = a;
        }
    }

    void emit_solid_rect( QuadTarget& qt, const CalloutRect& r, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca ) noexcept
    {
        emit_quad( qt,
                   static_cast<float>( r.x0 ), static_cast<float>( r.y0 ),
                   static_cast<float>( r.x1 ), static_cast<float>( r.y1 ),
                   qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v,
                   cr, cg, cb, ca );
    }

    void append_boxes_quads( QuadTarget& qt, const GpuBox *boxes, int n ) noexcept
    {
        constexpr float t = 1.0f;
        for ( int i = 0; i < n; ++i )
        {
            const GpuBox& b = boxes[i];
            if ( b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f )
                continue;
            emit_quad( qt, b.x0, b.y0, b.x1, b.y0 + t, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            emit_quad( qt, b.x0, b.y1 - t, b.x1, b.y1, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            emit_quad( qt, b.x0, b.y0, b.x0 + t, b.y1, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            emit_quad( qt, b.x1 - t, b.y0, b.x1, b.y1, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
        }
    }

    void append_callout_quads( QuadTarget& qt, const CalloutItem& c ) noexcept
    {
        for ( int s = 0; s < c.n_segs; ++s )
            emit_solid_rect( qt, c.segs[s], c.r, c.g, c.b, 255 );

        const CalloutRect fill { c.pill.x0 + 1, c.pill.y0 + 1, c.pill.x1 - 1, c.pill.y1 - 1 };
        emit_solid_rect( qt, fill, 16, 16, 20, 200 );
        emit_solid_rect( qt, { c.pill.x0, c.pill.y0, c.pill.x1, c.pill.y0 + 1 }, c.r, c.g, c.b, 255 );
        emit_solid_rect( qt, { c.pill.x0, c.pill.y1 - 1, c.pill.x1, c.pill.y1 }, c.r, c.g, c.b, 255 );
        emit_solid_rect( qt, { c.pill.x0, c.pill.y0 + 1, c.pill.x0 + 1, c.pill.y1 - 1 }, c.r, c.g, c.b, 255 );
        emit_solid_rect( qt, { c.pill.x1 - 1, c.pill.y0 + 1, c.pill.x1, c.pill.y1 - 1 }, c.r, c.g, c.b, 255 );

        int gx = c.text_x;
        int pos = 0;
        while ( pos < c.text_bytes )
        {
            const uint32_t cp = glyph_font::next_utf8( c.text, c.text_bytes, pos );
            if ( cp == 0 )
                break;
            const int gi = glyph_font::glyph_index( cp );
            float u0 = 0.0f;
            float v0 = 0.0f;
            float u1 = 0.0f;
            float v1 = 0.0f;
            glyph_font::glyph_uv( gi, u0, v0, u1, v1 );
            const float fx = static_cast<float>( gx );
            const float fy = static_cast<float>( c.text_y );
            emit_quad( qt, fx, fy, fx + static_cast<float>( glyph_font::kGlyphW ),
                       fy + static_cast<float>( glyph_font::kGlyphH ), u0, v0, u1, v1, 255, 255, 255, 255 );
            gx += glyph_font::kGlyphW;
        }
    }

    // Compile/link diagnostics: libmali returns non-zero handles even for
    // failed shaders — without these checks a broken program degrades
    // silently into "nothing is drawn".
    bool shader_ok( GLuint sh, const char *what, const Log& log )
    {
        GLint ok = 0;
        glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
        if ( ok )
            return true;
        char info[512];
        GLsizei n = 0;
        glGetShaderInfoLog( sh, sizeof( info ), &n, info );
        log.warn() << "gpu_draw: " << what << " shader compile failed:\n" << std::string_view( info, n ) << '\n';
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
    GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
    if ( vs == 0 || fs == 0 )
    {
        log_.warn() << "gpu_draw: glCreateShader failed\n";
        if ( vs != 0 )
            glDeleteShader( vs );
        if ( fs != 0 )
            glDeleteShader( fs );
        return false;
    }
    glShaderSource( vs, 1, &kVS, nullptr );
    glCompileShader( vs );
    glShaderSource( fs, 1, &kFS, nullptr );
    glCompileShader( fs );

    if ( !shader_ok( vs, "vertex", log_ ) || !shader_ok( fs, "fragment", log_ ) )
    {
        glDeleteShader( vs );
        glDeleteShader( fs );
        return false;
    }

    st_.prog = glCreateProgram();
    if ( st_.prog == 0 )
    {
        log_.warn() << "gpu_draw: glCreateProgram failed\n";
        glDeleteShader( vs );
        glDeleteShader( fs );
        return false;
    }
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
        log_.warn() << "gpu_draw: program link failed:\n" << std::string_view( log, n ) << '\n';
        glDeleteProgram( st_.prog );
        st_.prog = 0;
        glDeleteShader( vs );
        glDeleteShader( fs );
        return false;
    }
    glDeleteShader( vs );  // linked — shaders references not need more
    glDeleteShader( fs );

    st_.apos = glGetAttribLocation( st_.prog, "a_pos" );
    st_.auv = glGetAttribLocation( st_.prog, "a_uv" );
    st_.acol = glGetAttribLocation( st_.prog, "a_col" );
    st_.utex = glGetUniformLocation( st_.prog, "u_tex" );

    if ( st_.apos < 0 || st_.auv < 0 || st_.acol < 0 || st_.utex < 0 )
    {
        log_.warn() << "gpu_draw: shader attrib/uniform missing from linked program\n";
        glDeleteProgram( st_.prog );
        st_.prog = 0;
        return false;
    }
    glUseProgram( st_.prog );
    glUniform1i( st_.utex, 0 );

    return st_.prog != 0;
}

// Attribute setup for the batched vertex layout (20-byte stride: pos, uv, rgba).
void GpuBoxDrawer::setup_attribs() const
{
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glEnableVertexAttribArray( st_.apos );
    glVertexAttribPointer( st_.apos, 2, GL_FLOAT, GL_FALSE, sizeof( BatchVert ), nullptr );
    if ( st_.auv >= 0 )
    {
        glEnableVertexAttribArray( st_.auv );
        glVertexAttribPointer( st_.auv, 2, GL_FLOAT, GL_FALSE, sizeof( BatchVert ),
                               reinterpret_cast<void *>( offsetof( BatchVert, u ) ) );
    }
    if ( st_.acol >= 0 )
    {
        glEnableVertexAttribArray( st_.acol );
        glVertexAttribPointer( st_.acol, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( BatchVert ),
                               reinterpret_cast<void *>( offsetof( BatchVert, r ) ) );
    }
}

GpuBoxDrawer::~GpuBoxDrawer()
{
    done();
}

Status GpuBoxDrawer::init()
{
    done();
    st_ = State {};
    ready_ = false;

    const auto fail = [this]( const char *what )
    {
        done();  // init() error path: release everything created so far
        return make_status( Errc::Failed, what );
    };

    if ( !open_display() )
        return fail( "drawer: EGL/GBM display" );
    if ( !load_entry_points() )
        return fail( "drawer: EGL entry points" );
    if ( !create_context() )
        return fail( "drawer: GLES context" );
    if ( !make_program() )
    {
        log_.warn() << "drawer: program failed\n";
        return fail( "drawer: shader program" );
    }
    create_atlas();
    create_buffers();
    ready_ = true;
    return {};
}

bool GpuBoxDrawer::open_display()
{
    st_.get_platform_display = reinterpret_cast<P_GetPlatformDisplayEXT>( eglGetProcAddress( "eglGetPlatformDisplayEXT" ) );
    if ( !st_.get_platform_display )
    {
        log_.warn() << "drawer: eglGetPlatformDisplayEXT missing\n";
        return false;
    }
    // GBM display: the mali libgbm first, then the system one
    // (calibrated in the probe: mali's copy initializes cleanly).
    st_.dl_gbm = dlopen( "/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !st_.dl_gbm )
        st_.dl_gbm = dlopen( "libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !st_.dl_gbm )
    {
        log_.warn() << "drawer: libgbm not loadable\n";
        return false;
    }
    auto create_dev = reinterpret_cast<P_GbmCreateDevice>( dlsym( st_.dl_gbm, "gbm_create_device" ) );
    if ( !create_dev )
    {
        log_.warn() << "drawer: gbm_create_device missing\n";
        return false;
    }
    for ( const char *node : { "/dev/dri/renderD128", "/dev/dri/card0" } )
    {
        if ( try_node( node, create_dev ) )
            return true;
    }
    log_.warn() << "drawer: EGL/GBM init failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
    return false;
}

bool GpuBoxDrawer::try_node( const char *path, P_GbmCreateDevice create_dev )
{
    const int fd = open( path, O_RDWR | O_CLOEXEC );
    if ( fd < 0 )
        return false;
    void *dev = create_dev( fd );
    if ( !dev )
    {
        close( fd );
        return false;
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
        destroy_gbm_device( dev );
        close( fd );
        return false;
    }
    st_.dpy = d;
    st_.gbm_fd = fd;  // kept open: gbm_dev is bound to it
    st_.gbm_dev = dev;
    return true;
}

bool GpuBoxDrawer::load_entry_points()
{
    const char *exts = eglQueryString( st_.dpy, EGL_EXTENSIONS );
    if ( !has( exts, "EGL_EXT_image_dma_buf_import" ) || !has( exts, "EGL_KHR_image_base" ) )
    {
        log_.warn() << "drawer: no image import extensions\n";
        return false;
    }

    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (the probe proved libmali does
    // not export all of them as symbols).
    st_.create_image = reinterpret_cast<P_CreateImage>( eglGetProcAddress( "eglCreateImageKHR" ) );
    st_.destroy_image = reinterpret_cast<P_DestroyImage>( eglGetProcAddress( "eglDestroyImageKHR" ) );
    st_.create_sync = reinterpret_cast<P_CreateSync>( eglGetProcAddress( "eglCreateSyncKHR" ) );
    st_.destroy_sync = reinterpret_cast<P_DestroySync>( eglGetProcAddress( "eglDestroySyncKHR" ) );
    st_.client_wait = reinterpret_cast<P_ClientWaitSync>( eglGetProcAddress( "eglClientWaitSyncKHR" ) );
    st_.image_target_tex = reinterpret_cast<P_ImageTargetTexture2DOES>( eglGetProcAddress( "glEGLImageTargetTexture2DOES" ) );
    st_.gen_vao = reinterpret_cast<P_GenVertexArrays>( eglGetProcAddress( "glGenVertexArrays" ) );
    st_.del_vao = reinterpret_cast<P_DeleteVertexArrays>( eglGetProcAddress( "glDeleteVertexArrays" ) );
    st_.bind_vao = reinterpret_cast<P_BindVertexArray>( eglGetProcAddress( "glBindVertexArray" ) );

    if ( !st_.create_image || !st_.destroy_image || !st_.image_target_tex )
    {
        log_.warn() << "drawer: image entry points missing\n";
        return false;
    }
    st_.native_fence = has( exts, "EGL_ANDROID_native_fence_sync" ) && st_.create_sync && st_.client_wait;
    return true;
}

bool GpuBoxDrawer::create_context()
{
    EGLConfig cfg = nullptr;
    EGLint n = 0;

    const EGLint cattr[] = {
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_NONE
    };

    if ( !eglChooseConfig( st_.dpy, cattr, &cfg, 1, &n ) || n < 1 )
    {
        const EGLint cattr2[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        if ( !eglChooseConfig( st_.dpy, cattr2, &cfg, 1, &n ) || n < 1 )
        {
            log_.warn() << "drawer: eglChooseConfig failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
            return false;
        }
    }
    const EGLint c3[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    const EGLint c2[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE };
    st_.ctx = eglCreateContext( st_.dpy, cfg, EGL_NO_CONTEXT, c3 );
    if ( !st_.ctx )
        st_.ctx = eglCreateContext( st_.dpy, cfg, EGL_NO_CONTEXT, c2 );
    if ( !st_.ctx || !eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st_.ctx ) )
    {
        log_.warn() << "drawer: context failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    return true;
}

void GpuBoxDrawer::create_atlas()
{
    std::array<uint8_t, glyph_font::kAtlasBytes> atlas {};
    glyph_font::build_atlas( atlas );
    glGenTextures( 1, &st_.atlas_tex );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, st_.atlas_tex );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, glyph_font::kAtlasW, glyph_font::kAtlasH, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, atlas.data() );
    glBindTexture( GL_TEXTURE_2D, 0 );
}

void GpuBoxDrawer::create_buffers()
{
    glGenBuffers( 1, &st_.vbo );
    // Pre-allocate the batch VBO once: draw() only uploads into it.
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glBufferData( GL_ARRAY_BUFFER, kVboBytes, nullptr, GL_STREAM_DRAW );
    // VAO (ES3 only, when the entry points exist): attributes are
    // recorded once here; draw() then does a single glBindVertexArray.
    const char *glver = reinterpret_cast<const char *>( glGetString( GL_VERSION ) );
    st_.es3 = glver && strstr( glver, "ES 3." ) != nullptr && st_.gen_vao && st_.del_vao && st_.bind_vao;
    if ( st_.es3 )
    {
        st_.gen_vao( 1, &st_.vao );
        st_.bind_vao( st_.vao );
        setup_attribs();
        st_.bind_vao( 0 );
        log_.info() << "drawer: batch VAO ready (ES3)\n";
    }
}

void GpuBoxDrawer::destroy_gbm_device( void *dev ) const
{
    auto destroy_dev = reinterpret_cast<P_GbmDestroyDevice>( dlsym( st_.dl_gbm, "gbm_destroy_device" ) );
    if ( destroy_dev )
        destroy_dev( dev );
}

Status GpuBoxDrawer::attach_slot( int slot, int tensor_dma_fd, int w, int h, uint32_t stride )
{
    if ( !ready_ || slot < 0 || slot >= State::kSlots )
        return make_status( Errc::Failed, "drawer: attach_slot precondition" );
    State::SlotImg& si = st_.slot[slot];
    if ( si.img != EGL_NO_IMAGE_KHR )
        return {};  // idempotent: the pool attaches each slot once
    make_current();

    EGLint iattr[] = { 
        EGL_WIDTH,
        w,
        EGL_HEIGHT,
        h,
        EGL_LINUX_DRM_FOURCC_EXT,
        DRM_FORMAT_RGB888,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        tensor_dma_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>( stride ),
        EGL_NONE 
    };
    
    si.img = st_.create_image( st_.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, static_cast<EGLClientBuffer>( nullptr ), iattr );
    if ( si.img == EGL_NO_IMAGE_KHR )
    {
        log_.warn() << "drawer: import tensor failed (slot " << slot << ", 0x" << std::hex << eglGetError() << std::dec << ")\n";
        return make_status( Errc::Failed, "drawer: import tensor failed" );
    }
    for ( int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i ) {}  // bounded clear of prior GL errors
    glGenTextures( 1, &si.tex );
    glBindTexture( GL_TEXTURE_2D, si.tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    st_.image_target_tex( GL_TEXTURE_2D, static_cast<GLeglImageOES>( si.img ) );
    glGenFramebuffers( 1, &si.fbo );
    glBindFramebuffer( GL_FRAMEBUFFER, si.fbo );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, si.tex, 0 );
    const GLenum gl_err = glGetError();
    if ( gl_err != GL_NO_ERROR )
    {
        log_.warn() << "drawer: GL error attaching slot " << slot << " (0x" << std::hex << gl_err << std::dec << ")\n";
        glBindTexture( GL_TEXTURE_2D, 0 );
        glBindFramebuffer( GL_FRAMEBUFFER, 0 );
        release_slot( slot );
        return make_status( Errc::Failed, "drawer: GL attach error" );
    }
    const GLenum fst = glCheckFramebufferStatus( GL_FRAMEBUFFER );
    if ( fst != GL_FRAMEBUFFER_COMPLETE )
    {
        log_.warn() << "drawer: FBO incomplete (slot " << slot << ", status 0x" << std::hex << fst << std::dec << ")\n";
        glBindTexture( GL_TEXTURE_2D, 0 );
        glBindFramebuffer( GL_FRAMEBUFFER, 0 );
        release_slot( slot );
        return make_status( Errc::Failed, "drawer: FBO incomplete" );
    }
    glBindTexture( GL_TEXTURE_2D, 0 );
    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    if ( st_.w == 0 )
    {
        st_.w = w;
        st_.h = h;
        st_.stride = stride;
    }
    else if ( st_.w != w || st_.h != h || st_.stride != stride )
    {
        log_.warn() << "drawer: slot " << slot << " geometry mismatch: " << w << 'x' << h << " stride=" << stride
                  << " vs " << st_.w << 'x' << st_.h << " stride=" << st_.stride << '\n';
        release_slot( slot );
        return make_status( Errc::Failed, "drawer: slot geometry mismatch" );
    }
    log_.info() << "drawer: tensor attached (slot " << slot << ") " << w << 'x' << h << " stride=" << stride
              << " (native fence: " << ( st_.native_fence ? "yes" : "no" ) << ")\n";
    return {};
}

void GpuBoxDrawer::draw_slot( int slot, const GpuBox *boxes, int n, const CalloutList *callouts )
{
    const bool have_boxes = boxes != nullptr && n > 0;
    const bool have_callouts = callouts != nullptr && callouts->n > 0;
    if ( !ready_ || ( !have_boxes && !have_callouts ) )
        return;
    if ( slot < 0 || slot >= State::kSlots || st_.slot[slot].fbo == 0 )
        return;
    n = have_boxes ? std::min( n, static_cast<int>( GpuBoxList::cap ) ) : 0;
    make_current();

    // Batched quads in tensor coordinates. The viewport clips anything
    // outside the tensor, matching the CPU-path clamping.
    // Y orientation: memory row r maps to NDC y = -1 + 2*r/h — GL's origin
    // (bottom-left) IS memory row 0, so there is NO flip. The probe's
    // green-band test sat at the center row, which is symmetric under a
    // flip and could not catch this; a live frame did (boxes were mirrored
    // around y = h/2).
    float su = 0.0f;
    float sv = 0.0f;
    glyph_font::solid_uv( su, sv );
    QuadTarget qt { st_.vstage.data(), 0, 2.0f / static_cast<float>( st_.w ), 2.0f / static_cast<float>( st_.h ), su, sv };
    if ( have_boxes )
        append_boxes_quads( qt, boxes, n );
    if ( have_callouts )
    {
        for ( int k = 0; k < callouts->n; ++k )
            append_callout_quads( qt, callouts->d[k] );
    }
    const int cnt = qt.cnt;
    BatchVert* v = qt.v;
    if ( cnt == 0 )
        return;

    if ( st_.es3 )
        st_.bind_vao( st_.vao );
    glBindFramebuffer( GL_FRAMEBUFFER, st_.slot[slot].fbo );
    glViewport( 0, 0, st_.w, st_.h );
    glUseProgram( st_.prog );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, st_.atlas_tex );
    glUniform1i( st_.utex, 0 );
    glDisable( GL_DEPTH_TEST );
    // Alpha blending: the per-vertex alpha was uploaded
    // but ignored (blend disabled, the RGB888 FBO stores no alpha).
    // Porter-Duff over: the trail fade from BoxMapper (255..1)
    // and the a = 255 detections render as computed. The context
    // belongs to this drawer alone, so leaving blending on between
    // draws is safe; every draw sets the state before drawing.
    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

    if ( !st_.es3 )
        setup_attribs();  // ES3: attributes live in the VAO
    
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glBufferSubData( GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>( cnt ) * sizeof( BatchVert ), v );
    glDrawArrays( GL_TRIANGLES, 0, cnt );
    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    glFlush();
}

void GpuBoxDrawer::sync_cpu()
{
    if ( st_.dpy == EGL_NO_DISPLAY )
        return;
    // Unconditional make_current(): the fallback path below used to call
    // glFinish() without ensuring the context is current (the fence path
    // did it inside the branch). make_current() is a no-op when the
    // context is already current (the hot-path case), so this costs
    // nothing in the steady state and makes the contract total: sync_cpu()
    // is safe from any thread state where dpy is alive.
    make_current();
    if ( st_.native_fence )
    {
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
            log_.warn() << "drawer: fence wait timed out, glFinish fallback\n";
        }
    }
    // Known limit: glFinish() has no timeout. If the fence timed out
    // because the GPU is wedged, this call will likely hang too -- the
    // 100 ms cap bounds the fence path only. Documented as accepted:
    // there is no bounded glFinish in EGL/GLES, and skipping the sync
    // would read the tensor mid-write (worse than hanging).
    glFinish();
}

void GpuBoxDrawer::release_slot( int slot )
{
    State::SlotImg& si = st_.slot[slot];
    if ( si.fbo )
        glDeleteFramebuffers( 1, &si.fbo );
    if ( si.tex )
        glDeleteTextures( 1, &si.tex );
    if ( si.img != EGL_NO_IMAGE_KHR && st_.destroy_image )
        st_.destroy_image( st_.dpy, si.img );
    si = {};
}

void GpuBoxDrawer::done()
{
    if ( st_.dpy != EGL_NO_DISPLAY )
    {
        make_current();
        if ( st_.atlas_tex )
            glDeleteTextures( 1, &st_.atlas_tex );
        st_.atlas_tex = 0;
        if ( st_.vbo )
            glDeleteBuffers( 1, &st_.vbo );
        st_.vbo = 0;
        if ( st_.prog )
            glDeleteProgram( st_.prog );
        st_.prog = 0;
        if ( st_.vao && st_.del_vao )
            st_.del_vao( 1, &st_.vao );
        st_.vao = 0;
        for ( int i = 0; i < State::kSlots; ++i )
            release_slot( i );
        if ( st_.ctx )
        {
            eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
            eglDestroyContext( st_.dpy, st_.ctx );
            st_.ctx = nullptr;
        }
        eglTerminate( st_.dpy );
        st_.dpy = EGL_NO_DISPLAY;
    }
    // GBM device outlives EGL on its fd: destroy it first, then close the fd,
    // then drop the dlopen handle. All three were leaked by the old code on every done()
    // Unconditional: init may fail after dlopen/dlopen+open, and those handles must go too.
    if ( st_.dl_gbm )
    {
        if ( st_.gbm_dev )
        {
            destroy_gbm_device( st_.gbm_dev );
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
