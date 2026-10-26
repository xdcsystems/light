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
#include <string.h>
#include <unistd.h>

#include "gpu_box_drawer.hpp"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFull
#endif

namespace {

bool has(const char *exts, const char *name) {
    return exts && strstr(exts, name) != nullptr;
}

// Shader sources: file-local constexpr constants (statics rule).
constexpr const char* VS =
    "attribute vec2 a_pos; void main(){ gl_Position=vec4(a_pos,0.,1.); }";
constexpr const char* FS =
    "precision mediump float; uniform vec4 u_col; "
    "void main(){ gl_FragColor=u_col; }";

}  // anonymous namespace

void GpuBoxDrawer::make_current() {
    eglMakeCurrent(st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, st_.ctx);
}

bool GpuBoxDrawer::make_program() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, nullptr);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, nullptr);
    glCompileShader(fs);
    st_.prog = glCreateProgram();
    glAttachShader(st_.prog, vs);
    glAttachShader(st_.prog, fs);
    glLinkProgram(st_.prog);
    st_.ucol = glGetUniformLocation(st_.prog, "u_col");
    st_.apos = glGetAttribLocation(st_.prog, "a_pos");
    return st_.prog != 0;
}

// One filled quad in tensor coordinates. The viewport clips anything
// outside the tensor, matching the CPU-path clamping.
// Y orientation: memory row r maps to NDC y = -1 + 2*r/h — GL's origin
// (bottom-left) IS memory row 0, so there is NO flip. The probe's
// green-band test sat at the center row, which is symmetric under a
// flip and could not catch this; a live frame did (boxes were mirrored
// around y = h/2).
void GpuBoxDrawer::draw_quad(float x0, float y0, float x1, float y1) {
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
    const float nx0 = (x0 / (float)st_.w) * 2.0f - 1.0f;
    const float nx1 = (x1 / (float)st_.w) * 2.0f - 1.0f;
    const float ny0 = (y0 / (float)st_.h) * 2.0f - 1.0f;
    const float ny1 = (y1 / (float)st_.h) * 2.0f - 1.0f;
    const GLfloat q[12] = {
        nx0, ny0,  nx1, ny0,  nx0, ny1,
        nx1, ny0,  nx1, ny1,  nx0, ny1,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

GpuBoxDrawer::GpuBoxDrawer() {}

GpuBoxDrawer::~GpuBoxDrawer() {
    done();
}

bool GpuBoxDrawer::init() {
    done();
    st_ = State{};
    ready_ = false;

    st_.get_platform_display = (P_GetPlatformDisplayEXT)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!st_.get_platform_display) {
        std::cerr << "drawer: eglGetPlatformDisplayEXT missing\n";
        return false;
    }
    // GBM display: the mali libgbm first, then the system one
    // (calibrated in the probe: mali's copy initializes cleanly).
    st_.dl_gbm = dlopen("/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1",
                       RTLD_NOW | RTLD_GLOBAL);
    if (!st_.dl_gbm) st_.dl_gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!st_.dl_gbm) {
        std::cerr << "drawer: libgbm not loadable\n";
        return false;
    }
    typedef void *(*P_gbm_create_device)(int);
    P_gbm_create_device gbm_create_device_ =
        (P_gbm_create_device)dlsym(st_.dl_gbm, "gbm_create_device");
    if (!gbm_create_device_) {
        std::cerr << "drawer: gbm_create_device missing\n";
        return false;
    }
    for (const char *node : { "/dev/dri/renderD128", "/dev/dri/card0" }) {
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        void *dev = gbm_create_device_(fd);
        if (!dev) { close(fd); continue; }
        EGLDisplay d = st_.get_platform_display(EGL_PLATFORM_GBM_KHR, dev,
                                               nullptr);
        EGLint maj = 0, min = 0;
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) continue;
        st_.dpy = d;
        st_.gbm_fd = fd;      // kept open: gbm_dev is bound to it
        st_.gbm_dev = dev;
        break;
    }
    if (st_.dpy == EGL_NO_DISPLAY) {
        std::cerr << "drawer: EGL/GBM init failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }

    const char *exts = eglQueryString(st_.dpy, EGL_EXTENSIONS);
    if (!has(exts, "EGL_EXT_image_dma_buf_import") ||
        !has(exts, "EGL_KHR_image_base")) {
        std::cerr << "drawer: no image import extensions\n";
        return false;
    }

    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (the probe proved libmali does
    // not export all of them as symbols).
    st_.create_image = (P_CreateImage)eglGetProcAddress("eglCreateImageKHR");
    st_.destroy_image = (P_DestroyImage)eglGetProcAddress("eglDestroyImageKHR");
    st_.create_sync = (P_CreateSync)eglGetProcAddress("eglCreateSyncKHR");
    st_.destroy_sync = (P_DestroySync)eglGetProcAddress("eglDestroySyncKHR");
    st_.client_wait = (P_ClientWaitSync)eglGetProcAddress("eglClientWaitSyncKHR");
    st_.dup_fence = (P_DupNativeFenceFDANDROID)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    st_.image_target_tex = (P_ImageTargetTexture2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!st_.create_image || !st_.image_target_tex) {
        std::cerr << "drawer: image entry points missing\n";
        return false;
    }
    st_.native_fence = has(exts, "EGL_ANDROID_native_fence_sync") &&
                      st_.create_sync && st_.client_wait;

    EGLConfig cfg = nullptr;
    EGLint n = 0;
    const EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    if (!eglChooseConfig(st_.dpy, cattr, &cfg, 1, &n) || n < 1) {
        const EGLint cattr2[] = {EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES2_BIT, EGL_NONE};
        eglChooseConfig(st_.dpy, cattr2, &cfg, 1, &n);
    }
    const EGLint c3[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                         EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    const EGLint c2[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
    st_.ctx = eglCreateContext(st_.dpy, cfg, EGL_NO_CONTEXT, c3);
    if (!st_.ctx) st_.ctx = eglCreateContext(st_.dpy, cfg, EGL_NO_CONTEXT, c2);
    if (!st_.ctx || !eglMakeCurrent(st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   st_.ctx)) {
        std::cerr << "drawer: context failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }
    if (!make_program()) {
        std::cerr << "drawer: program failed\n";
        return false;
    }
    glGenBuffers(1, &st_.vbo);
    ready_ = true;
    return true;
}

bool GpuBoxDrawer::attachTensor(int tensorDmaFd, int w, int h,
                                uint32_t stride) {
    if (st_.dpy == EGL_NO_DISPLAY) return false;
    make_current();
    st_.w = w;
    st_.h = h;
    st_.stride = stride;

    EGLint iattr[] = {EGL_WIDTH, w, EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, /* DRM_FORMAT_RGB888 */ 0x34324752,
        EGL_DMA_BUF_PLANE0_FD_EXT, tensorDmaFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
    if (st_.img != EGL_NO_IMAGE_KHR && st_.destroy_image)
        st_.destroy_image(st_.dpy, st_.img);
    st_.img = st_.create_image(st_.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                             (EGLClientBuffer)nullptr, iattr);
    if (st_.img == EGL_NO_IMAGE_KHR) {
        std::cerr << "drawer: import tensor failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }
    if (st_.tex) glDeleteTextures(1, &st_.tex);
    glGenTextures(1, &st_.tex);
    glBindTexture(GL_TEXTURE_2D, st_.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    st_.image_target_tex(GL_TEXTURE_2D, (GLeglImageOES)st_.img);
    if (st_.fbo) glDeleteFramebuffers(1, &st_.fbo);
    glGenFramebuffers(1, &st_.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, st_.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, st_.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "drawer: FBO incomplete (0x" << std::hex
                  << glGetError() << std::dec << ")\n";
        return false;
    }
    std::cout << "drawer: tensor attached " << w << 'x' << h << " stride="
              << stride << " (native fence: "
              << (st_.native_fence ? "yes" : "no") << ")\n";
    return true;
}

void GpuBoxDrawer::draw(const GpuBox* boxes, int n) {
    if (st_.dpy == EGL_NO_DISPLAY || boxes == nullptr || n <= 0) return;
    make_current();
    glBindFramebuffer(GL_FRAMEBUFFER, st_.fbo);
    glViewport(0, 0, st_.w, st_.h);
    glUseProgram(st_.prog);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, st_.vbo);
    glEnableVertexAttribArray(st_.apos);
    glVertexAttribPointer(st_.apos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    const float t = 1.0f;   // outline thickness, tensor px
    for (int i = 0; i < n; ++i) {
        const GpuBox& b = boxes[i];
        // The dma-buf reads as BGR to the GPU: the tensor R must go into
        // the GL blue channel (probe calibration).
        glUniform4f(st_.ucol, b.b / 255.0f, b.g / 255.0f, b.r / 255.0f,
                    b.a / 255.0f);
        draw_quad(b.x0, b.y0, b.x1, b.y0 + t);   // top edge
        draw_quad(b.x0, b.y1 - t, b.x1, b.y1);   // bottom edge
        draw_quad(b.x0, b.y0, b.x0 + t, b.y1);   // left edge
        draw_quad(b.x1 - t, b.y0, b.x1, b.y1);   // right edge
    }
}

void GpuBoxDrawer::syncCpu() {
    if (st_.dpy == EGL_NO_DISPLAY) return;
    if (st_.native_fence) {
        make_current();
        EGLSyncKHR sync = st_.create_sync(st_.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            st_.client_wait(st_.dpy, sync, 0, EGL_FOREVER_KHR);
            if (st_.destroy_sync) st_.destroy_sync(st_.dpy, sync);
            return;
        }
    }
    glFinish();
}

void GpuBoxDrawer::done() {
    if (st_.dpy == EGL_NO_DISPLAY) return;   // never inited: nothing to release
    make_current();
    if (st_.fbo) glDeleteFramebuffers(1, &st_.fbo);
    if (st_.tex) glDeleteTextures(1, &st_.tex);
    if (st_.img != EGL_NO_IMAGE_KHR && st_.destroy_image)
        st_.destroy_image(st_.dpy, st_.img);
    st_.img = EGL_NO_IMAGE_KHR;
    st_.fbo = 0;
    st_.tex = 0;
    if (st_.ctx) {
        eglMakeCurrent(st_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(st_.dpy, st_.ctx);
        st_.ctx = nullptr;
    }
    if (st_.dpy != EGL_NO_DISPLAY) eglTerminate(st_.dpy);
    st_.dpy = EGL_NO_DISPLAY;
    ready_ = false;
}
