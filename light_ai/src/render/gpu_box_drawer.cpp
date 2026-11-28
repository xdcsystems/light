/* GPU box drawing (Mali G610) straight into the NPU input tensor.
 * Calibration facts from tools/egl_probe.cpp (RK3588, libmali -x11-gbm):
 *   - EGL platform: GBM (mali libgbm first, then the system libgbm).
 *   - dma-buf RGB888 import OK; 24bpp dumb pitch == w*3 (no alignment).
 *   - GL writes red into memory byte2, i.e. it sees the buffer as BGR:
 *     the desired tensor R goes into the GL BLUE uniform channel.
 *   - EGL_ANDROID_native_fence_sync is available and usable.
 * Extensions the Mali-G610 libmali lists and init() requires: EGL_KHR_surfaceless_context, EGL_KHR_image_base,
 * EGL_EXT_image_dma_buf_import and GL_OES_EGL_image. It also lists EGL_KHR_fence_sync, which sync_cpu() prefers.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <dlfcn.h>
#include <fcntl.h>
#include <initializer_list>
#include <linux/dma-buf.h>
#include <cstring>
#include <ios>
#include <unistd.h>
#include <string_view>
#include <drm/drm_fourcc.h>

#include "render/gpu_box_drawer.hpp"
#include "core/frame_view.hpp"
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
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif

// 100 ms cap for sync_cpu(): a wedged GPU must not hang the pipeline.
constexpr EGLTimeKHR kFenceWaitNs = 100ull * 1000 * 1000;

namespace
{
    struct EglUnbind
    {
        EGLDisplay dpy;
        ~EglUnbind() { eglMakeCurrent( dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT ); }
    };

    // name is a whole entry of the space-separated extension list: a longer name that contains it does not count.
    bool has( const char *exts, const char *name )
    {
        if ( exts == nullptr )
            return false;
        const std::string_view list( exts );
        const std::string_view want( name );
        for ( size_t pos = 0; pos < list.size(); )
        {
            const size_t end = std::min( list.find( ' ', pos ), list.size() );
            if ( list.substr( pos, end - pos ) == want )
                return true;
            pos = end + 1;
        }
        return false;
    }

    // Per-vertex color (normalized ubyte attribute) instead of the per-box
    // uniform: that is what allows all boxes to be batched into ONE draw
    // call. Channel order is GL semantics — see the r/b swap note in draw().
    constexpr const char *kVS =
            "precision highp float; attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col; "
            "varying vec2 v_uv; varying vec4 v_col; "
            "void main(){ gl_Position=vec4(a_pos,0.,1.); v_uv=a_uv; v_col=a_col; }";
    // The outlined text in two passes (#108): the atlas is GL_LUMINANCE_ALPHA, L = the
    // glyph fill, A = its baked dark outline, and the vertex alpha selects the plane:
    //   a = v_col.a * L + (1 - v_col.a) * A,  out = v_col.rgb * a + dst * (1 - a)
    // (premultiplied here, blended with GL_ONE, GL_ONE_MINUS_SRC_ALPHA). The outline pass
    // draws every glyph of a label with alpha 0 in the outline gray (a = A), then the fill
    // pass draws them again in white with alpha 255 (a = L): all outlines under all
    // fills, as the former 8 offset passes + 1 white pass, so the outline of a glyph
    // never covers the ink of its neighbour (one pass per glyph would: the padded quads
    // of adjacent glyphs overlap). Solid quads sample the block (L = 1, A = 0): a = v_col.a,
    // the plain "over".
    constexpr const char *kFS =
            "precision highp float; uniform sampler2D u_tex; "
            "varying vec2 v_uv; varying vec4 v_col; "
            "void main(){ vec4 t = texture2D(u_tex, v_uv); float a = v_col.a * t.r + (1.0 - v_col.a) * t.a; "
            "gl_FragColor = vec4(v_col.rgb * a, a); }";

    // One VBO for boxes (GpuBoxList::cap * kVertsPerBox) + callouts
    // (CalloutList::cap * kVertsPerCallout), allocated once in init().
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
        // Offscreen cull (M-10): a quad wholly outside the target costs
        // 6 vertices of the shared budget and rasterizes nothing.
        if ( nx1 <= -1.0f || nx0 >= 1.0f || ny1 <= -1.0f || ny0 >= 1.0f )
            return;
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
            const float x0 = std::floor( b.x0 );
            const float y0 = std::floor( b.y0 );
            const float x1 = std::ceil( b.x1 );
            const float y1 = std::ceil( b.y1 );
            emit_quad( qt, x0, y0, x1, y0 + t, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            if ( y1 - y0 > t )
                emit_quad( qt, x0, y1 - t, x1, y1, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            // Vertical bars stop short of the horizontal ones: each corner
            // pixel is blended once (M-10). Porter-Duff over is not
            // idempotent for a < 255 -- the semi-transparent trail dots had
            // bright double corners unlike draw_boxes_cpu.
            if ( y1 - t <= y0 + t )
                continue;  // <= 2 px tall: horizontal bar(s) cover it
            emit_quad( qt, x0, y0 + t, x0 + t, y1 - t, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
            if ( x1 - x0 > t )
                emit_quad( qt, x1 - t, y0 + t, x1, y1 - t, qt.solid_u, qt.solid_v, qt.solid_u, qt.solid_v, b.r, b.g, b.b, b.a );
        }
    }

    // One pass of the label text of c, one quad a glyph: the padded atlas rects scaled by
    // c.scale at float positions; GL_LINEAR does the resampling. The glyph walk is
    // glyph_font::for_each_glyph, shared with the CPU path. The color picks the plane (kFS).
    void append_text_pass( QuadTarget& qt, const CalloutItem& c, uint8_t gray, uint8_t a ) noexcept
    {
        const float sc = c.scale;
        glyph_font::for_each_glyph(
            c.text, c.text_bytes, static_cast<float>( c.text_x ), static_cast<float>( c.text_y ), sc,
            [&qt, sc, gray, a]( int gi, const glyph_font::GlyphMetric& m, float gx, float gy ) {
                float u0 = 0.0f;
                float v0 = 0.0f;
                float u1 = 0.0f;
                float v1 = 0.0f;
                glyph_font::glyph_uv( gi, u0, v0, u1, v1 );
                emit_quad( qt, gx, gy, gx + static_cast<float>( m.w ) * sc, gy + static_cast<float>( m.h ) * sc,
                           u0, v0, u1, v1, gray, gray, gray, a );
            },
            true );
    }

    // Connector, then the text: the baked dark outline (alpha 0 selects the A plane, #108),
    // then the white fill over it (alpha 255 selects L) -- all in one GPU draw call.
    void append_callout_quads( QuadTarget& qt, const CalloutItem& c ) noexcept
    {
        for ( int s = 0; s < c.n_segs; ++s )
            emit_solid_rect( qt, c.segs[s], c.r, c.g, c.b, 255 );
        append_text_pass( qt, c, CalloutConfig::kOutlineGray, 0 );
        append_text_pass( qt, c, 255, 255 );
    }

    // Bounded clear of prior GL errors: the next glGetError() then reports only what follows.
    void drain_gl_errors() noexcept
    {
        for ( int i = 0; i < 16; ++i )
        {
            if ( glGetError() == GL_NO_ERROR )
                break;
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

bool GpuBoxDrawer::make_current() noexcept  // NOLINT(readability-make-member-function-const)
{
    // Without a context the comparison below would match a thread that has none current and report success.
    if ( st_.ctx == EGL_NO_CONTEXT )
        return false;
    // Same-context eglMakeCurrent is not free in libmali; skip it when
    // the context is already current (the common case in the hot path).
    if ( eglGetCurrentContext() == st_.ctx )
        return true;
    if ( eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st_.ctx ) == EGL_TRUE )
        return true;
    log_.warn() << "drawer: eglMakeCurrent failed (0x" << std::hex << eglGetError() << std::dec << ")\n";
    return false;
}

bool GpuBoxDrawer::make_program()
{
    const GLuint vs = glCreateShader( GL_VERTEX_SHADER );
    const GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
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
    static_assert( static_cast<int>( GpuBoxDrawer::State::kSlots ) >= static_cast<int>( TensorSet::cap ),
                   "GpuBoxDrawer slot cache must cover TensorSet::cap" );
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
    if ( !check_gl_extensions() )
        return fail( "drawer: GL extensions" );
    if ( !make_program() )
    {
        log_.warn() << "drawer: program failed\n";
        return fail( "drawer: shader program" );
    }
    create_atlas();
    create_buffers();
    eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
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
    const auto create_dev = reinterpret_cast<P_GbmCreateDevice>( dlsym( st_.dl_gbm, "gbm_create_device" ) );
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
    // create_context() makes the context current without a surface.
    if ( !has( exts, "EGL_KHR_surfaceless_context" ) )
    {
        log_.warn() << "drawer: no EGL_KHR_surfaceless_context\n";
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
    // destroy_sync too: a fence created without it would leak one EGLSync per frame (M-10).
    // Prefer EGL_KHR_fence_sync (a plain fence: no fd is exported here), fall back to EGL_ANDROID_native_fence_sync
    // before glFinish(). The later assignment wins.
    if ( st_.create_sync && st_.client_wait && st_.destroy_sync )
    {
        if ( has( exts, "EGL_ANDROID_native_fence_sync" ) )
            st_.fence_type = EGL_SYNC_NATIVE_FENCE_ANDROID;
        if ( has( exts, "EGL_KHR_fence_sync" ) )
            st_.fence_type = EGL_SYNC_FENCE_KHR;
    }
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

// glEGLImageTargetTexture2DOES (the dma-buf slots) is GL_OES_EGL_image; the GL list needs the context current,
// so this runs after create_context(). eglGetProcAddress alone does not prove an extension.
bool GpuBoxDrawer::check_gl_extensions() const
{
    if ( has( reinterpret_cast<const char *>( glGetString( GL_EXTENSIONS ) ), "GL_OES_EGL_image" ) )
        return true;
    log_.warn() << "drawer: no GL_OES_EGL_image\n";
    return false;
}

void GpuBoxDrawer::create_atlas()
{
    glGenTextures( 1, &st_.atlas_tex );
    glActiveTexture( GL_TEXTURE0 );
    glBindTexture( GL_TEXTURE_2D, st_.atlas_tex );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    // GL_LINEAR: the 22 px atlas is resampled to the label size (#91 item 5). The quad
    // UVs run to the glyph rect's edges, so a fragment center may sit closer than half a
    // texel to an edge and GL_LINEAR then taps 1 texel outside the rect (never more:
    // no mipmaps), into the gap. The invariant (tools/gen_glyph_font.py, asserted
    // there): the outer ring of every rect is 0 in both planes and the gap is at least
    // 1 px of zeros, so no sample ever reads a neighbour's ink. The solid block is 4x4,
    // sampled at its center.
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    // (L, A) = (fill, outline) per texel: the shader reads them as .r and .a.
    glTexImage2D( GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, glyph_font::kAtlasW, glyph_font::kAtlasH, 0,
                  GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, glyph_font::atlas() );
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
    const auto destroy_dev = reinterpret_cast<P_GbmDestroyDevice>( dlsym( st_.dl_gbm, "gbm_destroy_device" ) );
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
    if ( !make_current() )
        return make_status( Errc::Failed, "drawer: eglMakeCurrent failed" );
    const EglUnbind unbind { st_.dpy };

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
    drain_gl_errors();
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
    if ( st_.w != 0 && ( st_.w != w || st_.h != h || st_.stride != stride ) )
    {
        log_.warn() << "drawer: slot " << slot << " geometry mismatch: " << w << 'x' << h << " stride=" << stride
                  << " vs " << st_.w << 'x' << st_.h << " stride=" << st_.stride << '\n';
        release_slot( slot );
        return make_status( Errc::Failed, "drawer: slot geometry mismatch" );
    }
    if ( st_.w == 0 )  // the first slot sets the geometry every later slot must match
    {
        st_.w = w;
        st_.h = h;
        st_.stride = stride;
    }
    log_.info() << "drawer: tensor attached (slot " << slot << ") " << w << 'x' << h << " stride=" << stride
              << " (fence: " << ( st_.fence_type == EGL_SYNC_NATIVE_FENCE_ANDROID ? "native"
                                  : ( st_.fence_type == EGL_SYNC_FENCE_KHR ? "khr" : "none" ) ) << ")\n";
    return {};
}

bool GpuBoxDrawer::draw_slot( int slot, const GpuBox *boxes, int n, const CalloutList *callouts ) noexcept
{
    const bool have_boxes = boxes != nullptr && n > 0;
    const bool have_callouts = callouts != nullptr && callouts->n > 0;
    if ( !ready_ || ( !have_boxes && !have_callouts ) )
        return true;
    if ( slot < 0 || slot >= State::kSlots || st_.slot[slot].fbo == 0 )
        return true;
    n = have_boxes ? std::min( n, static_cast<int>( GpuBoxList::cap ) ) : 0;
    if ( !make_current() )
        return false;

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
    const BatchVert* v = qt.v;
    if ( cnt == 0 )
        return true;

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
    // Porter-Duff over, premultiplied in the shader (kFS): the trail
    // fade from BoxMapper (255..1) and the a = 255 detections render
    // as computed. The context belongs to this drawer alone, so leaving
    // blending on between draws is safe; every draw sets the state before drawing.
    glEnable( GL_BLEND );
    glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );

    if ( !st_.es3 )
        setup_attribs();  // ES3: attributes live in the VAO
    
    glBindBuffer( GL_ARRAY_BUFFER, st_.vbo );
    glBufferSubData( GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>( cnt ) * sizeof( BatchVert ), v );
    glDrawArrays( GL_TRIANGLES, 0, cnt );
    glBindFramebuffer( GL_FRAMEBUFFER, 0 );
    // No glFlush() here: sync_cpu() follows every draw and submits the commands itself.
    return true;
}

bool GpuBoxDrawer::sync_cpu() noexcept
{
    if ( st_.dpy == EGL_NO_DISPLAY )
        return true;
    if ( !make_current() )
        return false;
    if ( st_.fence_type != 0 )
    {
        EGLSyncKHR sync = st_.create_sync( st_.dpy, st_.fence_type, nullptr );
        if ( sync != EGL_NO_SYNC_KHR )
        {
            const EGLint r = st_.client_wait( st_.dpy, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, kFenceWaitNs );
            st_.destroy_sync( st_.dpy, sync );
            if ( r == EGL_CONDITION_SATISFIED_KHR )
                return true;
            ++fence_tmo_cnt_;
            log_.warn() << "drawer: fence wait failed/timed out (rc=" << r << "), skipping glFinish\n";
            return false;
        }
        ++fence_tmo_cnt_;
        log_.warn() << "drawer: eglCreateSyncKHR failed (err=0x" << std::hex << eglGetError()
                    << std::dec << "), skipping glFinish\n";
        return false;
    }
    glFinish();
    return true;
}

void GpuBoxDrawer::release_slot( int slot, bool gl_current )
{
    State::SlotImg& si = st_.slot[slot];
    if ( gl_current && si.fbo )
        glDeleteFramebuffers( 1, &si.fbo );
    if ( gl_current && si.tex )
        glDeleteTextures( 1, &si.tex );
    if ( si.img != EGL_NO_IMAGE_KHR && st_.destroy_image )
        st_.destroy_image( st_.dpy, si.img );
    si = {};
}

void GpuBoxDrawer::release_context() const noexcept
{
    if ( st_.dpy != EGL_NO_DISPLAY )
        eglMakeCurrent( st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
    eglReleaseThread();
}

void GpuBoxDrawer::delete_gl_objects()
{
    if ( st_.atlas_tex )
        glDeleteTextures( 1, &st_.atlas_tex );
    if ( st_.vbo )
        glDeleteBuffers( 1, &st_.vbo );
    if ( st_.prog )
        glDeleteProgram( st_.prog );
    if ( st_.vao && st_.del_vao )
        st_.del_vao( 1, &st_.vao );
}

void GpuBoxDrawer::done()
{
    if ( st_.dpy != EGL_NO_DISPLAY )
    {
        // Without a current context no GL call is made; the context and display
        // teardown below still runs and takes the GL objects with it.
        const bool current = make_current();
        if ( current )
            delete_gl_objects();
        st_.atlas_tex = 0;
        st_.vbo = 0;
        st_.prog = 0;
        st_.vao = 0;
        for ( int i = 0; i < State::kSlots; ++i )
            release_slot( i, current );
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
