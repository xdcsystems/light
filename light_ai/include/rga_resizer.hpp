#pragma once
#include <cstdint>

// Ресайз/конверсия формата через im2d. Источник и приёмник - dma-buf fd.
class RgaResizer {
public:
    RgaResizer() = default;
    // src: NV12 (w_src x h_src), dst: RGB888 (w_dst x h_dst)
    bool resize(int src_fd, int w_src, int h_src,
                int dst_fd, int w_dst, int h_dst);
};
