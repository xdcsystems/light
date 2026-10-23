#include <iostream>
#include <cstring>
#include "rga_resizer.hpp"
#include "im2d.h"
#include "rga.h"

bool RgaResizer::resize(int src_fd, int w_src, int h_src,
                        int dst_fd, int w_dst, int h_dst) {
    rga_buffer_t src = wrapbuffer_fd(src_fd, w_src, h_src, RK_FORMAT_YCbCr_422_SP);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888);
    IM_STATUS st = imresize(src, dst);
    if ((int)st <= 0) {
        // In this librga build success codes are positive (1 = SUCCESS,
        // 2 = NOERROR); a non-positive value means failure.
        std::cerr << "RGA resize failed: " << imStrError(st) << '\n';
        return false;
    }
    return true;
}
