#pragma once
#include <cstdint>

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
class RgaResizer {
public:
    RgaResizer();
    // src: YUV422SP (w_src x h_src), dst: RGB888 (w_dst x h_dst).
    // aux: optional intermediate dma-buf for the two-pass path when the
    // scale ratio exceeds the RGA3 per-axis limit (1/8).
    bool resize(int src_fd, int w_src, int h_src,
                int dst_fd, int w_dst, int h_dst,
                int aux_fd, int w_aux, int h_aux);
};

