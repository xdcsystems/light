/* M3.7 разведка: EGL/GLES (Mali G610) + импорт dma-buf RGB888 + GL-запись + native fence.
 * Сборка: c++ -O2 tools/egl_probe.cpp -o /tmp/egl_probe $(pkg-config --cflags --libs egl glesv2) -ldl
 * Запуск: sudo LD_LIBRARY_PATH=/usr/local/lib /tmp/egl_probe
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <initializer_list>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef fourcc_code
#define fourcc_code( a, b, c, d ) \
    ( ( uint32_t )( a ) | ( ( uint32_t )( b ) << 8 ) | ( ( uint32_t )( c ) << 16 ) | ( ( uint32_t )( d ) << 24 ) )
#endif
#ifndef DRM_FORMAT_RGB888
#define DRM_FORMAT_RGB888 fourcc_code( 'R', 'G', '2', '4' )
#endif
#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
/* DRM_IOCTL_MODE_MAP_DUMB: выдать mmap-смещение dumb-буфера */
#ifndef DRM_IOCTL_MODE_MAP_DUMB
struct drm_mode_map_dumb
{
    uint32_t handle, pad;
    uint64_t offset;
};
#define M37_IOCTL( n, t ) _IOWR( 'd', 0x40 + ( n ), t ) /* 0x40 = DRM_COMMAND_BASE */
#define DRM_IOCTL_MODE_MAP_DUMB M37_IOCTL( 0x73, struct drm_mode_map_dumb )
#endif

/* --- типы точек входа расширений (получаем через eglGetProcAddress) --- */
typedef EGLImageKHR( EGLAPIENTRYP P_eglCreateImageKHR )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
typedef EGLBoolean( EGLAPIENTRYP P_eglDestroyImageKHR )( EGLDisplay, EGLImageKHR );
typedef EGLSyncKHR( EGLAPIENTRYP P_eglCreateSyncKHR )( EGLDisplay, EGLenum, const EGLint * );
typedef EGLBoolean( EGLAPIENTRYP P_eglDestroySyncKHR )( EGLDisplay, EGLSyncKHR );
typedef EGLint( EGLAPIENTRYP P_eglDupNativeFenceFDANDROID )( EGLDisplay, EGLSyncKHR );
typedef EGLDisplay( EGLAPIENTRYP P_eglGetPlatformDisplayEXT )( EGLenum, void *, const EGLint * );
typedef void( GL_APIENTRYP P_glEGLImageTargetTexture2DOES )( GLenum, GLeglImageOES );
typedef void( GL_APIENTRYP P_glEGLImageTargetRenderbufferStorageOES )( GLenum, GLeglImageOES );

static P_eglCreateImageKHR fn_eglCreateImageKHR;
static P_eglDestroyImageKHR fn_eglDestroyImageKHR;
static P_eglCreateSyncKHR fn_eglCreateSyncKHR;
static P_eglDestroySyncKHR fn_eglDestroySyncKHR;
static P_eglDupNativeFenceFDANDROID fn_eglDupNativeFenceFDANDROID;
static P_eglGetPlatformDisplayEXT fn_eglGetPlatformDisplayEXT;
static P_glEGLImageTargetTexture2DOES fn_glEGLImageTargetTexture2DOES;
static P_glEGLImageTargetRenderbufferStorageOES fn_glEGLImageTargetRenderbufferStorageOES;

static bool has( const char *exts, const char *name )
{
    return exts && strstr( exts, name ) != nullptr;
}
static void buf_sync( int fd, bool read, bool begin )
{
    dma_buf_sync s {};
    s.flags = ( read ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_WRITE ) | ( begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END );
    ioctl( fd, DMA_BUF_IOCTL_SYNC, &s );
}
static void dump_rows( const uint8_t *m, uint32_t stride, int bpp, const char *tag )
{
    const int px = bpp / 8;
    printf( "%s (stride=%u, bpp=%d):\n", tag, stride, bpp );
    const int rows[] = { 0, 1, 319, 320, 321, 639 };
    for ( int y : rows )
    {
        printf( "  row %3d:", y );
        for ( int x : { 0, 320, 639 } )
        {
            size_t off = (size_t)y * stride + (size_t)x * px;
            printf( "  (%3u,%3u,%3u)", m[off], m[off + 1], m[off + 2] );
        }
        printf( "\n" );
    }
}

static const char *VS = "attribute vec2 a_pos; void main(){ gl_Position=vec4(a_pos,0.,1.); }";
static const char *FS = "precision mediump float; uniform vec4 u_col; void main(){ gl_FragColor=u_col; }";

int main( void )
{
    /* 1. Клиентские расширения (до eglInitialize) */
    const char *client_exts = eglQueryString( EGL_NO_DISPLAY, EGL_EXTENSIONS );
    printf( "client exts: %s\n", client_exts ? client_exts : "(none)" );

    /* 2. Display: default -> X11 -> GBM (libmali -x11-gbm требует явную платформу) */
    EGLint maj = 0, min = 0;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    const char *how = "default";

    fn_eglGetPlatformDisplayEXT = (P_eglGetPlatformDisplayEXT)eglGetProcAddress( "eglGetPlatformDisplayEXT" );

    dpy = eglGetDisplay( EGL_DEFAULT_DISPLAY );
    if ( dpy != EGL_NO_DISPLAY && !eglInitialize( dpy, &maj, &min ) )
    {
        eglTerminate( dpy );
        dpy = EGL_NO_DISPLAY;
    }

    if ( dpy == EGL_NO_DISPLAY && fn_eglGetPlatformDisplayEXT )
    {
        /* X11: сначала default, затем явный дисплей :1 (под sudo DISPLAY теряется) */
        void *hX = dlopen( "libX11.so.6", RTLD_NOW | RTLD_GLOBAL );
        if ( hX )
        {
            typedef void *( *P_XOpenDisplay )( const char * );
            P_XOpenDisplay XOpenDisplay_ = (P_XOpenDisplay)dlsym( hX, "XOpenDisplay" );
            for ( const char *name : { (const char *)nullptr, ":1" } )
            {
                if ( !XOpenDisplay_ )
                    break;
                void *xdisp = XOpenDisplay_( name );
                if ( !xdisp )
                    continue;
                dpy = fn_eglGetPlatformDisplayEXT( EGL_PLATFORM_X11_KHR, xdisp, nullptr );
                if ( dpy == EGL_NO_DISPLAY || !eglInitialize( dpy, &maj, &min ) )
                    dpy = EGL_NO_DISPLAY;
                else
                {
                    how = "x11";
                    break;
                }
            }
        }
    }
    if ( dpy == EGL_NO_DISPLAY && fn_eglGetPlatformDisplayEXT )
    {
        /* GBM: сначала малиевский libgbm, затем системный */
        void *hG = dlopen( "/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
        if ( !hG )
            hG = dlopen( "libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
        if ( hG )
        {
            typedef void *( *P_gbm_create_device )( int );
            P_gbm_create_device gbm_create_device_ = (P_gbm_create_device)dlsym( hG, "gbm_create_device" );
            for ( const char *node : { "/dev/dri/renderD128", "/dev/dri/card0" } )
            {
                if ( !gbm_create_device_ )
                    break;
                int gfd = open( node, O_RDWR | O_CLOEXEC );
                if ( gfd < 0 )
                    continue;
                void *gdev = gbm_create_device_( gfd );
                if ( !gdev )
                {
                    close( gfd );
                    continue;
                }
                dpy = fn_eglGetPlatformDisplayEXT( EGL_PLATFORM_GBM_KHR, gdev, nullptr );
                if ( dpy == EGL_NO_DISPLAY || !eglInitialize( dpy, &maj, &min ) )
                {
                    dpy = EGL_NO_DISPLAY;
                    continue;
                }
                how = "gbm";
                break;
            }
        }
    }

    if ( dpy == EGL_NO_DISPLAY )
    {
        printf( "FATAL: eglInitialize failed на всех платформах (0x%x)\n", eglGetError() );
        return 1;
    }
    printf( "EGL %d.%d vendor='%s' version='%s'\n", maj, min, eglQueryString( dpy, EGL_VENDOR ), eglQueryString( dpy, EGL_VERSION ) );
    printf( "display: инициализирован через %s\n", how );
    const char *disp_exts = eglQueryString( dpy, EGL_EXTENSIONS );
    printf( "display exts: %s\n", disp_exts );

    struct
    {
        const char *name;
        bool critical;
    } exts[] = {
            { "EGL_EXT_image_dma_buf_import", true },
            { "EGL_EXT_image_dma_buf_import_modifiers", false },
            { "EGL_KHR_image_base", true },
            { "EGL_KHR_image", true },
            { "EGL_KHR_surfaceless_context", false },
            { "EGL_KHR_fence_sync", false },
            { "EGL_ANDROID_native_fence_sync", false },
    };
    for ( auto &e : exts )
        printf( "  %-42s %s%s\n",
                e.name,
                has( disp_exts, e.name ) ? "YES" : "no",
                ( !has( disp_exts, e.name ) && e.critical ) ? "  <-- CRITICAL" : "" );

    /* 2b. Точки входа расширений — через eglGetProcAddress */
    fn_eglCreateImageKHR = (P_eglCreateImageKHR)eglGetProcAddress( "eglCreateImageKHR" );
    fn_eglDestroyImageKHR = (P_eglDestroyImageKHR)eglGetProcAddress( "eglDestroyImageKHR" );
    fn_eglCreateSyncKHR = (P_eglCreateSyncKHR)eglGetProcAddress( "eglCreateSyncKHR" );
    fn_eglDestroySyncKHR = (P_eglDestroySyncKHR)eglGetProcAddress( "eglDestroySyncKHR" );
    fn_eglDupNativeFenceFDANDROID = (P_eglDupNativeFenceFDANDROID)eglGetProcAddress( "eglDupNativeFenceFDANDROID" );
    fn_glEGLImageTargetTexture2DOES = (P_glEGLImageTargetTexture2DOES)eglGetProcAddress( "glEGLImageTargetTexture2DOES" );
    fn_glEGLImageTargetRenderbufferStorageOES =
            (P_glEGLImageTargetRenderbufferStorageOES)eglGetProcAddress( "glEGLImageTargetRenderbufferStorageOES" );

    printf( "entry points:\n" );
    printf( "  eglCreateImageKHR:                %s\n", fn_eglCreateImageKHR ? "OK" : "MISSING" );
    printf( "  eglDestroyImageKHR:               %s\n", fn_eglDestroyImageKHR ? "OK" : "MISSING" );
    printf( "  eglCreateSyncKHR:                 %s\n", fn_eglCreateSyncKHR ? "OK" : "MISSING" );
    printf( "  eglDestroySyncKHR:                %s\n", fn_eglDestroySyncKHR ? "OK" : "MISSING" );
    printf( "  eglDupNativeFenceFDANDROID:       %s\n", fn_eglDupNativeFenceFDANDROID ? "OK" : "MISSING" );
    printf( "  glEGLImageTargetTexture2DOES:     %s\n", fn_glEGLImageTargetTexture2DOES ? "OK" : "MISSING" );
    printf( "  glEGLImageTargetRenderbufferStorageOES: %s\n", fn_glEGLImageTargetRenderbufferStorageOES ? "OK" : "MISSING" );

    if ( !fn_eglCreateImageKHR || !fn_glEGLImageTargetTexture2DOES )
    {
        printf( "FATAL: нет точек входа image-расширений\n" );
        return 1;
    }

    /* 3. Config + context (GLES3 -> GLES2 fallback) */
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
    if ( !eglChooseConfig( dpy, cattr, &cfg, 1, &n ) || n < 1 )
    {
        const EGLint cattr2[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
        eglChooseConfig( dpy, cattr2, &cfg, 1, &n );
    }
    EGLContext ctx = nullptr;
    int gl_ver = 3;
    const EGLint c3[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
    const EGLint c2[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE };
    ctx = eglCreateContext( dpy, cfg, EGL_NO_CONTEXT, c3 );
    if ( !ctx )
    {
        ctx = eglCreateContext( dpy, cfg, EGL_NO_CONTEXT, c2 );
        gl_ver = 2;
    }
    if ( !ctx )
    {
        printf( "FATAL: no EGL context (0x%x)\n", eglGetError() );
        return 1;
    }

    /* 4. Surfaceless, иначе 1x1 pbuffer */
    if ( !eglMakeCurrent( dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx ) )
    {
        printf( "surfaceless failed, pbuffer fallback (0x%x)\n", eglGetError() );
        const EGLint pattr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        EGLSurface surf = eglCreatePbufferSurface( dpy, cfg, pattr );
        if ( !surf || !eglMakeCurrent( dpy, surf, surf, ctx ) )
        {
            printf( "FATAL: makeCurrent failed\n" );
            return 1;
        }
    }
    printf( "GL: vendor='%s' renderer='%s' version='%s' (requested GLES%d)\n",
            (const char *)glGetString( GL_VENDOR ),
            (const char *)glGetString( GL_RENDERER ),
            (const char *)glGetString( GL_VERSION ),
            gl_ver );
    const char *gl_exts = (const char *)glGetString( GL_EXTENSIONS );
    printf( "  GL_OES_EGL_image: %s\n", has( gl_exts, "GL_OES_EGL_image" ) ? "YES" : "no" );
    printf( "  GL_OES_EGL_image_external: %s\n", has( gl_exts, "GL_OES_EGL_image_external" ) ? "YES" : "no" );

    /* 5. dma-buf RGB888 через DRM dumb buffer: import -> GL write -> CPU read */
    int drm_fd = -1, dfd = -1, bpp = 24;
    uint32_t pitch = 0, handle = 0;
    for ( const char *d : { "/dev/dri/card0", "/dev/dri/card1" } )
    {
        int fd = open( d, O_RDWR | O_CLOEXEC );
        if ( fd < 0 )
            continue;
        drm_mode_create_dumb cd {}; /* bpp=24 -> RGB888, как тензор */
        cd.width = 640;
        cd.height = 640;
        cd.bpp = 24;
        cd.flags = 0;
        if ( ioctl( fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd ) != 0 || cd.pitch == 0 )
        {
            drm_mode_create_dumb cd32 {}; /* fallback: XRGB8888, 4 байта/пиксель */
            cd32.width = 640;
            cd32.height = 640;
            cd32.bpp = 32;
            cd32.flags = 0;
            if ( ioctl( fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd32 ) != 0 || cd32.pitch == 0 )
            {
                close( fd );
                continue;
            }
            cd = cd32;
            bpp = 32;
        }
        drm_prime_handle ph {};
        ph.handle = cd.handle;
        ph.fd = -1;
        if ( ioctl( fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph ) != 0 )
        {
            close( fd );
            continue;
        }
        drm_fd = fd;
        dfd = ph.fd;
        pitch = cd.pitch;
        handle = cd.handle;
        printf( "dumb buffer: dev=%s bpp=%d pitch=%u size=%llu\n", d, bpp, pitch, (unsigned long long)cd.size );
        break;
    }
    if ( drm_fd < 0 )
    {
        printf( "FATAL: dumb buffer не создался ни на одной карте (см. dmesg)\n" );
        return 0;
    }
    /* mmap: dumb-буфер мапится по drm-fd через MAP_DUMB-оффсет (не по dma-buf fd) */
    size_t sz = (size_t)pitch * 640;
    drm_mode_map_dumb md {};
    md.handle = handle;
    if ( ioctl( drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md ) != 0 )
    {
        printf( "FATAL: DRM_IOCTL_MODE_MAP_DUMB failed\n" );
        return 1;
    }
    uint8_t *m = (uint8_t *)mmap( nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, (off_t)md.offset );
    if ( m == MAP_FAILED )
    {
        printf( "FATAL: mmap dumb failed\n" );
        return 1;
    }

    buf_sync( dfd, false, true );
    memset( m, 0, sz );
    buf_sync( dfd, false, false );

    uint32_t fourcc = ( bpp == 24 ) ? DRM_FORMAT_RGB888 : fourcc_code( 'X', 'R', '2', '4' ); /* XRGB8888 */
    EGLint iattr[] = { EGL_WIDTH,
            640,
            EGL_HEIGHT,
            640,
            EGL_LINUX_DRM_FOURCC_EXT,
            (EGLint)fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT,
            dfd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            (EGLint)pitch,
            EGL_NONE };
    EGLImageKHR img = fn_eglCreateImageKHR( dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, ( EGLClientBuffer ) nullptr, iattr );
    if ( img == EGL_NO_IMAGE_KHR )
    {
        printf( "FATAL: eglCreateImageKHR failed (0x%x)\n", eglGetError() );
        return 1;
    }
    printf( "EGLImage: imported OK\n" );

    GLuint tex, fbo;
    glGenTextures( 1, &tex );
    glBindTexture( GL_TEXTURE_2D, tex );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    fn_glEGLImageTargetTexture2DOES( GL_TEXTURE_2D, (GLeglImageOES)img );
    glGenFramebuffers( 1, &fbo );
    glBindFramebuffer( GL_FRAMEBUFFER, fbo );
    glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
    if ( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
    {
        printf( "tex-FBO incomplete (0x%x), fallback renderbuffer\n", glGetError() );
        GLuint rbo;
        glGenRenderbuffers( 1, &rbo );
        glBindRenderbuffer( GL_RENDERBUFFER, rbo );
        if ( fn_glEGLImageTargetRenderbufferStorageOES )
            fn_glEGLImageTargetRenderbufferStorageOES( GL_RENDERBUFFER, (GLeglImageOES)img );
        glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo );
    }
    if ( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
    {
        printf( "FATAL: FBO not complete (0x%x)\n", glGetError() );
        return 1;
    }

    GLuint vs = glCreateShader( GL_VERTEX_SHADER );
    glShaderSource( vs, 1, &VS, nullptr );
    glCompileShader( vs );
    GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
    glShaderSource( fs, 1, &FS, nullptr );
    glCompileShader( fs );
    GLuint prog = glCreateProgram();
    glAttachShader( prog, vs );
    glAttachShader( prog, fs );
    glLinkProgram( prog );
    GLint ucol = glGetUniformLocation( prog, "u_col" );
    GLint apos = glGetAttribLocation( prog, "a_pos" );

    glUseProgram( prog );
    glViewport( 0, 0, 640, 640 );
    /* Тест A: clear красным -> порядок байтов в памяти */
    glUniform4f( ucol, 1.0f, 0.0f, 0.0f, 1.0f );
    GLfloat full[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo;
    glGenBuffers( 1, &vbo );
    glBindBuffer( GL_ARRAY_BUFFER, vbo );
    glEnableVertexAttribArray( apos );
    glVertexAttribPointer( apos, 2, GL_FLOAT, GL_FALSE, 0, nullptr );
    glBufferData( GL_ARRAY_BUFFER, sizeof( full ), full, GL_STATIC_DRAW );
    glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

    /* Тест B: зелёная полоса ровно в row=320 -> точность и питч */
    glUniform4f( ucol, 0.0f, 1.0f, 0.0f, 1.0f );
    GLfloat line[] = { -1, 0.0f, 1, 0.0f, -1, -0.003125f, 1, -0.003125f };
    glBindBuffer( GL_ARRAY_BUFFER, vbo );
    glBufferData( GL_ARRAY_BUFFER, sizeof( line ), line, GL_STATIC_DRAW );
    glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
    glFinish();

    buf_sync( dfd, true, true );
    dump_rows( m, pitch, bpp, "after GL draw (expect: row 320 = GREEN, остальн. = RED)" );
    buf_sync( dfd, true, false );

    /* 6. Native fence */
    if ( has( disp_exts, "EGL_ANDROID_native_fence_sync" ) && fn_eglCreateSyncKHR )
    {
        EGLSyncKHR s = fn_eglCreateSyncKHR( dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr );
        if ( s != EGL_NO_SYNC_KHR )
        {
            int f = fn_eglDupNativeFenceFDANDROID ? (int)fn_eglDupNativeFenceFDANDROID( dpy, s ) : -1;
            printf( "native fence: fd=%d %s\n", f, f >= 0 ? "(usable)" : "(dup failed)" );
            if ( f >= 0 )
                close( f );
            if ( fn_eglDestroySyncKHR )
                fn_eglDestroySyncKHR( dpy, s );
        }
        else
            printf( "native fence: eglCreateSyncKHR failed (0x%x)\n", eglGetError() );
    }
    else
        printf( "native fence: нет EGL_ANDROID_native_fence_sync или точки входа\n" );

    if ( fn_eglDestroyImageKHR )
        fn_eglDestroyImageKHR( dpy, img );
    printf( "DONE\n" );
    return 0;
}
