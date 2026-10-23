#include <cstring>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "pipeline.hpp"
#include "coco_names.h"
#include "yolo_postprocess.hpp"

namespace {

using clk = std::chrono::steady_clock;

inline double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Timings {
    double cam = 0, dec = 0, rga = 0, infer = 0; // sums, ms
    int n = 0;

    void add(double c, double d, double r, double i) {
        cam += c; dec += d; rga += r; infer += i;
        if (++n % 60 == 0) report("avg/60");
    }

    void report(const char* tag) {
        if (n == 0) return;
        const double sum = (cam + dec + rga + infer) / n;
        std::cout << "[timing] " << tag
                  << " cam=" << std::fixed << std::setprecision(1) << cam / n
                  << " dec=" << dec / n
                  << " rga=" << rga / n
                  << " infer=" << infer / n
                  << " | sum=" << sum << " ms -> ~"
                  << (sum > 0 ? 1000.0 / sum : 0.0) << " FPS\n";
        cam = dec = rga = infer = 0;
        n = 0;
    }
} g_t;

} // namespace

Pipeline::Pipeline(const Config& cfg)
    : cfg_(cfg),
      cam_(cfg_.device, cfg_.cam_w, cfg_.cam_h),
      dec_(cfg_.cam_w, cfg_.cam_h),
      npu_(cfg_.model.c_str()) {
    if (npu_.setup_io() != 0) {
        throw std::runtime_error("rknn setup_io failed");
    }
    // Single source of truth for the model geometry: the model file itself.
    // NHWC layout: dims = {N, H, W, C}, so W is dims[n-2] and H is dims[n-3].
    const rknn_tensor_attr& ia = npu_.input_attr();
    if (ia.n_dims < 4) {
        std::cerr << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
        throw std::runtime_error("rknn: input tensor is not 4-D NHWC");
    }

    // Gray letterbox bars: RGA writes only the visible band (drect), so the
    // 128-fill done here survives every frame.
    std::memset(npu_.input_data(), 128, npu_.input_size());

    model_w_ = static_cast<int>(ia.dims[ia.n_dims - 2]);
    model_h_ = static_cast<int>(ia.dims[ia.n_dims - 3]);

    // Aux buffer only for the two-pass path (source over 8x the model input).
    // 1280x720 -> 224 is 5.7x — single pass, aux not allocated, no DRM pool
    // split. 1920x1080 still needs the 960x540 intermediate.
    if (cfg_.cam_w > 8 * model_w_ || cfg_.cam_h > 8 * model_h_) {
        aux_fd_ = dec_.aux_fd(static_cast<size_t>(cfg_.cam_w / 2) * (cfg_.cam_h / 2) * 2);
        if (aux_fd_ < 0) {
            throw std::runtime_error("pipeline: aux buffer alloc failed");
        }
    } else {
        std::cout << "aux not needed: " << cfg_.cam_w << 'x' << cfg_.cam_h
                  << " fits single-pass into " << model_w_ << 'x' << model_h_ << '\n';
    }

    cam_.start();
}

int Pipeline::run() {
    int ok = 0;
    const auto t_start = clk::now();

    for (int i = 0; i < cfg_.frames; ++i) {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        const int idx = cam_.dequeue(&sz);
        if (idx < 0) break;
        const auto t1 = clk::now();

        const uint8_t* jpeg = static_cast<const uint8_t*>(cam_.buffer(idx));
        const int fd_src = dec_.decode(jpeg, sz);
        cam_.requeue(idx);
        const auto t2 = clk::now();

        // A frame that failed to decode contributes neither to ok nor to the
        // timing statistics — otherwise rare errors would silently skew averages.
        if (fd_src < 0) continue;

        const bool rga_ok = rga_.resize(fd_src, cfg_.cam_w, cfg_.cam_h,
                                        npu_.input_fd(), model_w_, model_h_,
                                        aux_fd_, cfg_.cam_w / 2, cfg_.cam_h / 2);

        const auto t3 = clk::now();

        if (rga_ok && cfg_.dump_cal && !cal_done_) {
            // The only diagnostic CPU pixel read: one model-input frame as
            // P6 PPM. RGB888, w_stride=0, so bytes = W*H*3 exactly.
            const size_t bytes = static_cast<size_t>(model_w_) * model_h_ * 3;
            std::ofstream out("/tmp/cal.ppm", std::ios::binary);
            if (out && npu_.input_size() >= bytes) {
                out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
                out.write(reinterpret_cast<const char*>(npu_.input_data()),
                          static_cast<std::streamsize>(bytes));
                std::cout << "calibration dump: /tmp/cal.ppm ("
                          << model_w_ << 'x' << model_h_ << ")\n";
            } else {
                std::cerr << "calibration dump failed\n";
            }
            cal_done_ = true;
        }

        const bool npu_ok = rga_ok && (npu_.run() == 0);
        const auto t4 = clk::now();

        if (npu_ok) {
            // Letterbox geometry from the single source of truth: RgaResizer
            // stored it during the last resize.
            const auto dets = yolo_postprocess(npu_, cfg_.cam_w, cfg_.cam_h,
                                               model_w_, model_h_,
                                               static_cast<float>(rga_.lb_top()),
                                               static_cast<float>(rga_.lb_h()),
                                               cfg_.conf_thr, cfg_.iou_thr);
            // Throttled report: every 15th frame, up to 8 boxes per report.
            if (!dets.empty() && i % 15 == 0) {
                std::cout << "frame " << i << ": " << dets.size()
                          << " det(s)\n";
                for (size_t k = 0; k < dets.size() && k < 8; ++k) {
                    const auto& d = dets[k];
                    std::cout << "  cls=" << kCocoNames[d.cls] << " conf="
                              << std::fixed << std::setprecision(2) << d.conf
                              << " box=(" << d.x << ',' << d.y << ' '
                              << d.w << 'x' << d.h << ")\n";
                }
            }
            ++ok;
        }
        g_t.add(ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4));
    }

    const double wall = std::chrono::duration<double>(clk::now() - t_start).count();
    g_t.report("TOTAL ");
    std::cout << "Processed " << ok << " frames in "
              << std::fixed << std::setprecision(3) << wall << " s -> "
              << (wall > 0 ? ok / wall : 0.0) << " FPS\n";
    return ok;
}
