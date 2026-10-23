#include <iostream>
#include <cstring>
#include "rga_resizer.hpp"
#include "im2d.h"
#include "rga.h"


RgaResizer::RgaResizer() {
    // RK3588: RGA3 (own IOMMU) + RGA2 (32-bit phys addressing, cannot map
    // dma-bufs above 4G). 1080p decoder buffers land above 4G — dmesg:
    // "rga_mm: RGA_MMU unsupported memory larger than 4G!", scheduler fell
    // back to RGA2 (core[4]). All jobs are pinned to RGA3 core0 for the
    // process lifetime; RGA2 is never needed here.
    const IM_STATUS st = imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    if ((int)st <= 0) {
        std::cerr << "RGA: imconfig RGA3_CORE0 pin failed: " << imStrError(st) << '\n';
    }
}

bool RgaResizer::resize(int src_fd, int w_src, int h_src,
                        int dst_fd, int w_dst, int h_dst,
                        int aux_fd, int w_aux, int h_aux) {
    // RGA3 scales at most 1/8 per axis. NOTE: integer division hides the
    // limit (1920/224 == 8, but the real ratio is 8.57) — compare by
    // multiplication.
    if (w_src > 8 * w_dst || h_src > 8 * h_dst) {
        rga_buffer_t src = wrapbuffer_fd(src_fd, w_src, h_src, RK_FORMAT_YCbCr_422_SP);
        rga_buffer_t mid = wrapbuffer_fd(aux_fd, w_aux, h_aux, RK_FORMAT_YCbCr_422_SP);
        IM_STATUS st = imresize(src, mid);
        if ((int)st <= 0) {
            std::cerr << "RGA resize (pass 1) failed: " << imStrError(st) << '\n';
            return false;
        }
        src_fd = aux_fd;
        w_src = w_aux;
        h_src = h_aux;
    }
    rga_buffer_t src = wrapbuffer_fd(src_fd, w_src, h_src, RK_FORMAT_YCbCr_422_SP);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, w_dst, h_dst, RK_FORMAT_RGB_888);
    IM_STATUS st = imresize(src, dst);
    if ((int)st <= 0) {
        std::cerr << "RGA resize failed: " << imStrError(st) << '\n';
        return false;
    }
    return true;
}

