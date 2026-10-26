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

typedef EGLImageKHR (EGLAPIENTRYP P_CreateImage)
    (EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (EGLAPIENTRYP P_DestroyImage)(EGLDisplay, EGLImageKHR);
typedef EGLSyncKHR (EGLAPIENTRYP P_CreateSync)(EGLDisplay, EGLenum,
                                               const EGLint *);
typedef EGLBoolean (EGLAPIENTRYP P_DestroySync)(EGLDisplay, EGLSyncKHR);
typedef EGLBoolean (EGLAPIENTRYP P_ClientWaitSync)(EGLDisplay, EGLSyncKHR,
                                                   EGLint, EGLTimeKHR);
typedef EGLint (EGLAPIENTRYP P_DupNativeFenceFDANDROID)(EGLDisplay, EGLSyncKHR);
typedef EGLDisplay (EGLAPIENTRYP P_GetPlatformDisplayEXT)
    (EGLenum, void *, const EGLint *);
typedef void (GL_APIENTRYP P_ImageTargetTexture2DOES)(GLenum, GLeglImageOES);

bool has(const char *exts, const char *name) {
    return exts && strstr(exts, name) != nullptr;
}

struct State {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = nullptr;
    EGLImageKHR img = EGL_NO_IMAGE_KHR;
    GLuint tex = 0, fbo = 0, prog = 0, vbo = 0;
    GLint ucol = -1, apos = -1;
    int w = 0, h = 0;
    uint32_t stride = 0;
    int gbm_fd = -1;            // the GBM device lives on this fd
    void *gbm_dev = nullptr;
    void *dl_gbm = nullptr;
    bool native_fence = false;

    P_CreateImage create_image = nullptr;
    P_DestroyImage destroy_image = nullptr;
    P_CreateSync create_sync = nullptr;
    P_DestroySync destroy_sync = nullptr;
    P_ClientWaitSync client_wait = nullptr;
    P_DupNativeFenceFDANDROID dup_fence = nullptr;  // future RGA handoff
    P_GetPlatformDisplayEXT get_platform_display = nullptr;
    P_ImageTargetTexture2DOES image_target_tex = nullptr;
};

void make_current(State& s) {
    eglMakeCurrent(s.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s.ctx);
}

namespace {

// Shader sources: file-local constexpr constants (statics rule).
constexpr const char* VS =
    "attribute vec2 a_pos; void main(){ gl_Position=vec4(a_pos,0.,1.); }";
constexpr const char* FS =
    "precision mediump float; uniform vec4 u_col; "
    "void main(){ gl_FragColor=u_col; }";

}  // anonymous namespace

bool make_program(State& s) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, nullptr);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, nullptr);
    glCompileShader(fs);
    s.prog = glCreateProgram();
    glAttachShader(s.prog, vs);
    glAttachShader(s.prog, fs);
    glLinkProgram(s.prog);
    s.ucol = glGetUniformLocation(s.prog, "u_col");
    s.apos = glGetAttribLocation(s.prog, "a_pos");
    return s.prog != 0;
}

// One filled quad in tensor coordinates. The viewport clips anything
// outside the tensor, matching the CPU-path clamping.
// Y orientation: memory row r maps to NDC y = -1 + 2*r/h — GL's origin
// (bottom-left) IS memory row 0, so there is NO flip. The probe's
// green-band test sat at the center row, which is symmetric under a
// flip and could not catch this; a live frame did (boxes were mirrored
// around y = h/2).
void draw_quad(State& s, float x0, float y0, float x1, float y1) {
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
    const float nx0 = (x0 / (float)s.w) * 2.0f - 1.0f;
    const float nx1 = (x1 / (float)s.w) * 2.0f - 1.0f;
    const float ny0 = (y0 / (float)s.h) * 2.0f - 1.0f;
    const float ny1 = (y1 / (float)s.h) * 2.0f - 1.0f;
    const GLfloat q[12] = {
        nx0, ny0,  nx1, ny0,  nx0, ny1,
        nx1, ny0,  nx1, ny1,  nx0, ny1,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

} // namespace

GpuBoxDrawer::GpuBoxDrawer() : st_(nullptr) {}

GpuBoxDrawer::~GpuBoxDrawer() {
    done();
    delete static_cast<State*>(st_);
}

bool GpuBoxDrawer::init() {
    done();
    delete static_cast<State*>(st_);
    st_ = nullptr;
    ready_ = false;

    State* s = new State();
    st_ = s;

    s->get_platform_display = (P_GetPlatformDisplayEXT)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!s->get_platform_display) {
        std::cerr << "drawer: eglGetPlatformDisplayEXT missing\n";
        return false;
    }
    // GBM display: the mali libgbm first, then the system one
    // (calibrated in the probe: mali's copy initializes cleanly).
    s->dl_gbm = dlopen("/usr/lib/aarch64-linux-gnu/mali/libgbm.so.1",
                       RTLD_NOW | RTLD_GLOBAL);
    if (!s->dl_gbm) s->dl_gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!s->dl_gbm) {
        std::cerr << "drawer: libgbm not loadable\n";
        return false;
    }
    typedef void *(*P_gbm_create_device)(int);
    P_gbm_create_device gbm_create_device_ =
        (P_gbm_create_device)dlsym(s->dl_gbm, "gbm_create_device");
    if (!gbm_create_device_) {
        std::cerr << "drawer: gbm_create_device missing\n";
        return false;
    }
    for (const char *node : { "/dev/dri/renderD128", "/dev/dri/card0" }) {
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        void *dev = gbm_create_device_(fd);
        if (!dev) { close(fd); continue; }
        EGLDisplay d = s->get_platform_display(EGL_PLATFORM_GBM_KHR, dev,
                                               nullptr);
        EGLint maj = 0, min = 0;
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) continue;
        s->dpy = d;
        s->gbm_fd = fd;      // kept open: gbm_dev is bound to it
        s->gbm_dev = dev;
        break;
    }
    if (s->dpy == EGL_NO_DISPLAY) {
        std::cerr << "drawer: EGL/GBM init failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }

    const char *exts = eglQueryString(s->dpy, EGL_EXTENSIONS);
    if (!has(exts, "EGL_EXT_image_dma_buf_import") ||
        !has(exts, "EGL_KHR_image_base")) {
        std::cerr << "drawer: no image import extensions\n";
        return false;
    }

    // Extension entry points: never assumed to be linkable, always via
    // eglGetProcAddress with a null check (the probe proved libmali does
    // not export all of them as symbols).
    s->create_image = (P_CreateImage)eglGetProcAddress("eglCreateImageKHR");
    s->destroy_image = (P_DestroyImage)eglGetProcAddress("eglDestroyImageKHR");
    s->create_sync = (P_CreateSync)eglGetProcAddress("eglCreateSyncKHR");
    s->destroy_sync = (P_DestroySync)eglGetProcAddress("eglDestroySyncKHR");
    s->client_wait = (P_ClientWaitSync)eglGetProcAddress("eglClientWaitSyncKHR");
    s->dup_fence = (P_DupNativeFenceFDANDROID)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    s->image_target_tex = (P_ImageTargetTexture2DOES)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!s->create_image || !s->image_target_tex) {
        std::cerr << "drawer: image entry points missing\n";
        return false;
    }
    s->native_fence = has(exts, "EGL_ANDROID_native_fence_sync") &&
                      s->create_sync && s->client_wait;

    EGLConfig cfg = nullptr;
    EGLint n = 0;
    const EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    if (!eglChooseConfig(s->dpy, cattr, &cfg, 1, &n) || n < 1) {
        const EGLint cattr2[] = {EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES2_BIT, EGL_NONE};
        eglChooseConfig(s->dpy, cattr2, &cfg, 1, &n);
    }
    const EGLint c3[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                         EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    const EGLint c2[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
    s->ctx = eglCreateContext(s->dpy, cfg, EGL_NO_CONTEXT, c3);
    if (!s->ctx) s->ctx = eglCreateContext(s->dpy, cfg, EGL_NO_CONTEXT, c2);
    if (!s->ctx || !eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   s->ctx)) {
        std::cerr << "drawer: context failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }
    if (!make_program(*s)) {
        std::cerr << "drawer: program failed\n";
        return false;
    }
    glGenBuffers(1, &s->vbo);
    ready_ = true;
    return true;
}

bool GpuBoxDrawer::attachTensor(int tensorDmaFd, int w, int h,
                                uint32_t stride) {
    State* s = static_cast<State*>(st_);
    if (!s || s->dpy == EGL_NO_DISPLAY) return false;
    make_current(*s);
    s->w = w;
    s->h = h;
    s->stride = stride;

    EGLint iattr[] = {EGL_WIDTH, w, EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, /* DRM_FORMAT_RGB888 */ 0x34324752,
        EGL_DMA_BUF_PLANE0_FD_EXT, tensorDmaFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride, EGL_NONE};
    if (s->img != EGL_NO_IMAGE_KHR && s->destroy_image)
        s->destroy_image(s->dpy, s->img);
    s->img = s->create_image(s->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                             (EGLClientBuffer)nullptr, iattr);
    if (s->img == EGL_NO_IMAGE_KHR) {
        std::cerr << "drawer: import tensor failed (0x" << std::hex
                  << eglGetError() << std::dec << ")\n";
        return false;
    }
    if (s->tex) glDeleteTextures(1, &s->tex);
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    s->image_target_tex(GL_TEXTURE_2D, (GLeglImageOES)s->img);
    if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "drawer: FBO incomplete (0x" << std::hex
                  << glGetError() << std::dec << ")\n";
        return false;
    }
    std::cout << "drawer: tensor attached " << w << 'x' << h << " stride="
              << stride << " (native fence: "
              << (s->native_fence ? "yes" : "no") << ")\n";
    return true;
}

void GpuBoxDrawer::draw(const std::vector<GpuBox>& boxes) {
    State* s = static_cast<State*>(st_);
    if (!s || s->dpy == EGL_NO_DISPLAY || boxes.empty()) return;
    make_current(*s);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, s->w, s->h);
    glUseProgram(s->prog);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glEnableVertexAttribArray(s->apos);
    glVertexAttribPointer(s->apos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    const float t = 1.0f;   // outline thickness, tensor px
    for (const GpuBox& b : boxes) {
        // The dma-buf reads as BGR to the GPU: the tensor R must go into
        // the GL blue channel (probe calibration).
        glUniform4f(s->ucol, b.b / 255.0f, b.g / 255.0f, b.r / 255.0f,
                    b.a / 255.0f);
        draw_quad(*s, b.x0, b.y0, b.x1, b.y0 + t);   // top edge
        draw_quad(*s, b.x0, b.y1 - t, b.x1, b.y1);   // bottom edge
        draw_quad(*s, b.x0, b.y0, b.x0 + t, b.y1);   // left edge
        draw_quad(*s, b.x1 - t, b.y0, b.x1, b.y1);   // right edge
    }
}

void GpuBoxDrawer::syncCpu() {
    State* s = static_cast<State*>(st_);
    if (!s || s->dpy == EGL_NO_DISPLAY) return;
    if (s->native_fence) {
        make_current(*s);
        EGLSyncKHR sync = s->create_sync(s->dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            s->client_wait(s->dpy, sync, 0, EGL_FOREVER_KHR);
            if (s->destroy_sync) s->destroy_sync(s->dpy, sync);
            return;
        }
    }
    glFinish();
}

void GpuBoxDrawer::done() {
    if (!st_) return;
    State* s = static_cast<State*>(st_);
    make_current(*s);
    if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
    if (s->tex) glDeleteTextures(1, &s->tex);
    if (s->img != EGL_NO_IMAGE_KHR && s->destroy_image)
        s->destroy_image(s->dpy, s->img);
    s->img = EGL_NO_IMAGE_KHR;
    s->fbo = 0;
    s->tex = 0;
    if (s->ctx) {
        eglMakeCurrent(s->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(s->dpy, s->ctx);
        s->ctx = nullptr;
    }
    if (s->dpy != EGL_NO_DISPLAY) eglTerminate(s->dpy);
    s->dpy = EGL_NO_DISPLAY;
    ready_ = false;
}
