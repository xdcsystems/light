/* rga_fill_probe: какие форматы проходят через RGA fill/rectangle
 * на этой плате (RK3588, RGA3x2 + RGA2E). Ожидание по гипотезе:
 * fill RGB888 — "invalid function policy", NV12/RGBA8888 — OK.
 * Сборка: c++ -O2 tools/rga_fill_probe.cpp -o /tmp/rga_fill_probe \
 *   $(pkg-config --cflags --libs librga)
 */
#include <cstddef>   /* NULL — im2d_single.h uses it without including */
#include <cstring>   /* memset */
#include <im2d.h>
#include <rga.h>
#include <cstdio>
#include <vector>

static void test(const char *tag, int w, int h, int fmt, int color) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4 + 4096);
    rga_buffer_t rb = wrapbuffer_virtualaddr(buf.data(), w, h, fmt);
    im_rect r{0, 0, w, h};
    IM_STATUS st = imfill(rb, r, color);
    printf("%-30s -> %s\n", tag, st <= 0 ? imStrError(st) : "OK");
}

int main() {
    printf("librga version: %s\n", querystring(RGA_VERSION));
    test("fill RGB888 640x640",      640, 640, RK_FORMAT_RGB_888,   0x40ff40);
    test("fill RGBA8888 640x640",    640, 640, RK_FORMAT_RGBA_8888, 0xff40ff40);
    test("fill RGB565 640x640",      640, 640, RK_FORMAT_RGB_565,   0x40ff40);
    test("fill NV12 640x640",        640, 640, RK_FORMAT_YCbCr_420_SP, 0x40ff40);
    // Репро реального кейса: тонкая полоса нечётной ширины
    {
        std::vector<uint8_t> buf(640 * 640 * 3 + 4096);
        rga_buffer_t rb = wrapbuffer_virtualaddr(buf.data(), 640, 640,
                                                 RK_FORMAT_RGB_888);
        im_rect r{238, 391, 51, 2};
        IM_STATUS st = imfill(rb, r, 0x40ff40);
        printf("%-30s -> %s\n", "fill RGB888 51x2 strip",
               st <= 0 ? imStrError(st) : "OK");
    }
    return 0;
}
