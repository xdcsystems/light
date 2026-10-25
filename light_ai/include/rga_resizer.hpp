#pragma once
#include <cstdint>
#include <vector>

#include "yolo_postprocess.hpp"   // Detection

// Resize/format conversion via im2d. Source and destination are dma-buf fds.
// The destination is letterboxed: uniform scale by width (s = w_dst/w_src),
// the visible band goes into rows [lb_top(), lb_top()+lb_h()) of the
// destination; the remaining rows keep what was there before (Pipeline
// memsets them to 128 once at startup).
class RgaResizer {
public:
    RgaResizer();
    // src: YUV422SP (w_src x h_src), dst: RGB888 (w_dst x h_dst).
    // aux: optional intermediate dma-buf for the two-pass path when the
    // scale ratio exceeds the RGA3 per-axis limit (1/8); pass -1 if unused.
    bool resize(int src_fd, int w_src, int h_src,
                int dst_fd, int w_dst, int h_dst,
                int aux_fd, int w_aux, int h_aux);

    // Letterbox geometry of the last resize — for the detection mapping T:
    // x_scene = x_det * w_src / w_dst; y_scene = (y_det - lb_top()) * w_src / w_dst;
    // a detection with y_det < lb_top() or >= lb_top() + lb_h() sits on a bar.
    int lb_top() const { return lb_top_; }
    int lb_h()   const { return lb_h_; }

    // M3.6: draws 1-px detection borders directly on the tensor
    // dma-buf (RGB888) via RGA imrectangle(). Coordinates: scene ->
    // model via model_per_scene, y shifted by lb_top; rects are
    // clipped to [lb_top, lb_top+lb_h) so the letterbox bars stay
    // clean for the next inference.
    bool draw_boxes(int tensor_fd, int w, int h,
                    const std::vector<Detection>& dets,
                    float model_per_scene, int lb_top);

private:
    int lb_top_ = 0;
    int lb_h_   = 0;
};

