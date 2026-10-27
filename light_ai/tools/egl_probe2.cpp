/* egl_probe2: разделение причин EGL_BAD_ALLOC (0x3003) при импорте
 * dma-buf в EGLImage (Mali G610).
 *   A: dumb 224x224 RGB888 (pitch 672, размер 150528 = 36.75 стр)  — репро
 *   B: dumb 224x256 RGB888 (pitch 672, размер 172032 = 42 стр)     — импорт как 224x224
 *   C: dumb 256x224 RGB888 (pitch 768 = 64x12, размер 172032)      — импорт как 224x224
 *   D: реальный rknn-тензор из модели (путь в argv[1])
 * Сборка: c++ -O2 tools/egl_probe2.cpp -o /tmp/egl_probe2 \
 *   $(pkg-config --cflags --libs egl glesv2) -ldl -lrknnrt \
 *   -I/mnt/nvme_data/work/rknn_model_zoo/3rdparty/rknpu2/include
 * Запуск: sudo LD_LIBRARY_PATH=/usr/local/lib /tmp/egl_probe2 [model.rknn]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm/drm.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "rknn_api.h"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
#ifndef DRM_FORMAT_RGB888
#define fourcc_code( a, b, c, d ) \
    ( ( uint32_t )( a ) | ( ( uint32_t )( b ) << 8 ) | ( ( uint32_t )( c ) << 16 ) | ( ( uint32_t )( d ) << 24 ) )
#define DRM_FORMAT_RGB888 fourcc_code( 'R', 'G', '2', '4' )
#endif

typedef EGLImageKHR( EGLAPIENTRYP P_CreateImage )( EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint * );
typedef EGLDisplay( EGLAPIENTRYP P_GetPlatformDisplayEXT )( EGLenum, void *, const EGLint * );

static P_CreateImage fn_create_image;
static EGLDisplay dpy;

static bool import_test( const char *tag, int dfd, int w, int h, uint32_t pitch )
{
    EGLint attr[] = { EGL_WIDTH,
            w,
            EGL_HEIGHT,
            h,
            EGL_LINUX_DRM_FOURCC_EXT,
            (EGLint)DRM_FORMAT_RGB888,
            EGL_DMA_BUF_PLANE0_FD_EXT,
            dfd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            (EGLint)pitch,
            EGL_NONE };
    EGLImageKHR img = fn_create_image( dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr );
    printf( "%-30s w=%4d h=%4d pitch=%5u -> %s (err 0x%x)\n",
            tag,
            w,
            h,
            pitch,
            img != EGL_NO_IMAGE_KHR ? "OK  " : "FAIL",
            img == EGL_NO_IMAGE_KHR ? (unsigned)eglGetError() : 0u );
    return img != EGL_NO_IMAGE_KHR;
}

static int prime_fd( int fd, uint32_t handle )
{
    drm_prime_handle ph {};
    ph.handle = handle;
    ph.fd = -1;
    if ( ioctl( fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph ) != 0 )
        return -1;
    return ph.fd;
}

struct Dumb
{
    int fd = -1, prime = -1;
    uint32_t handle = 0, pitch = 0;
    uint64_t size = 0;
};

static Dumb make_dumb( int w, int h, int bpp )
{
    Dumb d;
    for ( const char *node : { "/dev/dri/card0", "/dev/dri/card1" } )
    {
        int fd = open( node, O_RDWR | O_CLOEXEC );
        if ( fd < 0 )
            continue;
        drm_mode_create_dumb cd {};
        cd.width = w;
        cd.height = h;
        cd.bpp = bpp;
        cd.flags = 0;
        if ( ioctl( fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd ) != 0 || cd.pitch == 0 )
        {
            close( fd );
            continue;
        }
        int p = prime_fd( fd, cd.handle );
        if ( p < 0 )
        {
            close( fd );
            continue;
        }
        d.fd = fd;
        d.prime = p;
        d.handle = cd.handle;
        d.pitch = cd.pitch;
        d.size = cd.size;
        return d;
    }
    return d;
}

static void close_dumb( Dumb &d )
{
    if ( d.prime >= 0 )
        close( d.prime );
    if ( d.fd >= 0 )
        close( d.fd ); /* handle освобождается при close */
    d = Dumb {};
}

int main( int argc, char **argv )
{
    /* EGL/GBM — ровно как в drawer'е */
    P_GetPlatformDisplayEXT getPD = (P_GetPlatformDisplayEXT)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
    void *hG = dlopen( "/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !hG )
        hG = dlopen( "libgbm.so.1", RTLD_NOW | RTLD_GLOBAL );
    if ( !hG )
    {
        printf( "FATAL: no libgbm\n" );
        return 1;
    }
    typedef void *( *P_gbm_create_device )( int );
    P_gbm_create_device gbm_create_device_ = (P_gbm_create_device)dlsym( hG, "gbm_create_device" );
    int gfd = open( "/dev/dri/renderD128", O_RDWR | O_CLOEXEC );
    if ( gfd < 0 )
        gfd = open( "/dev/dri/card0", O_RDWR | O_CLOEXEC );
    void *gdev = gbm_create_device_( gfd );
    dpy = getPD( EGL_PLATFORM_GBM_KHR, gdev, nullptr );
    EGLint maj = 0, min = 0;
    if ( dpy == EGL_NO_DISPLAY || !eglInitialize( dpy, &maj, &min ) )
    {
        printf( "FATAL: EGL/GBM init failed\n" );
        return 1;
    }
    fn_create_image = (P_CreateImage)eglGetProcAddress( "eglCreateImageKHR" );
    if ( !fn_create_image )
    {
        printf( "FATAL: no eglCreateImageKHR\n" );
        return 1;
    }

    /* A: точная геометрия тензора 224x224 — ожидаем повтор 0x3003 */
    Dumb a = make_dumb( 224, 224, 24 );
    printf( "A dumb 224x224: pitch=%u size=%llu (%%4096=%llu, pitch%%64=%u)\n",
            a.pitch,
            (unsigned long long)a.size,
            (unsigned long long)( a.size % 4096 ),
            a.pitch % 64 );
    bool a_ok = a.prime >= 0 && import_test( "A import 224x224/672", a.prime, 224, 224, a.pitch );

    /* B: тот же stride, но буфер добит до границы страницы */
    Dumb b = make_dumb( 224, 256, 24 );
    printf( "B dumb 224x256: pitch=%u size=%llu (%%4096=%llu)\n",
            b.pitch,
            (unsigned long long)b.size,
            (unsigned long long)( b.size % 4096 ) );
    bool b_ok = b.prime >= 0 && import_test( "B import 224x256 as 224/672", b.prime, 224, 224, b.pitch );

    /* C: stride кратен 64 (768) и размер нацелен */
    Dumb c = make_dumb( 256, 224, 24 );
    printf( "C dumb 256x224: pitch=%u size=%llu (%%4096=%llu, pitch%%64=%u)\n",
            c.pitch,
            (unsigned long long)c.size,
            (unsigned long long)( c.size % 4096 ),
            c.pitch % 64 );
    bool c_ok = c.prime >= 0 && import_test( "C import 256x224 as 224/768", c.prime, 224, 224, c.pitch );

    /* D: настоящий rknn-тензор из модели */
    const char *model = argc > 1 ? argv[1] : "/usr/share/model/RK3588/mobilenet_v1.rknn";
    FILE *fp = fopen( model, "rb" );
    if ( !fp )
    {
        printf( "D skipped: model '%s' not openable\n", model );
    }
    else
    {
        fseek( fp, 0, SEEK_END );
        long len = ftell( fp );
        fseek( fp, 0, SEEK_SET );
        std::vector<char> data( len );
        fread( data.data(), 1, len, fp );
        fclose( fp );

        rknn_context ctx = 0;
        if ( rknn_init( &ctx, data.data(), len, 0, nullptr ) != 0 )
        {
            printf( "D skipped: rknn_init failed\n" );
        }
        else
        {
            rknn_tensor_attr attr {};
            attr.index = 0;
            if ( rknn_query( ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof( attr ) ) != 0 )
            {
                printf( "D skipped: rknn_query INPUT_ATTR failed\n" );
            }
            else
            {
                uint32_t w = attr.dims[attr.n_dims - 2];
                uint32_t h = attr.dims[attr.n_dims - 3];
                uint32_t pitch = attr.size_with_stride / h;
                rknn_tensor_mem *m = rknn_create_mem( ctx, attr.size_with_stride );
                if ( !m )
                {
                    printf( "D skipped: rknn_create_mem failed\n" );
                }
                else
                {
                    printf( "D rknn tensor: w=%u h=%u pitch=%u size=%u "
                            "(%%4096=%llu, pitch%%64=%u)\n",
                            w,
                            h,
                            pitch,
                            attr.size_with_stride,
                            (unsigned long long)( attr.size_with_stride % 4096 ),
                            pitch % 64 );
                    import_test( "D import rknn tensor", (int)m->fd, (int)w, (int)h, pitch );
                    rknn_destroy_mem( ctx, m );
                }
            }
            rknn_destroy( ctx );
        }
    }

    close_dumb( a );
    close_dumb( b );
    close_dumb( c );
    printf( "DONE: A=%s B=%s C=%s\n", a_ok ? "OK" : "FAIL", b_ok ? "OK" : "FAIL", c_ok ? "OK" : "FAIL" );
    return 0;
}
