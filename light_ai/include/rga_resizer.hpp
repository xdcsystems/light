#pragma once
#include <cstdint>

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
class RgaResizer {
public:
    RgaResizer() = default;
    // src: YUV422SP (w_src x h_src), dst: RGB888 (w_dst x h_dst)
    // NOTE: wrapbuffer_fd assumes wstride == w. This holds for w % 16 == 0
    // (640 OK). For widths not aligned to 16 the MPP-allocated buffer has an
    // aligned stride and the two-arg-overload with explicit wstride must be
    // used here.
    bool resize(int src_fd, int w_src, int h_src,
                int dst_fd, int w_dst, int h_dst);
};
