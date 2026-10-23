#include "pipeline.hpp"
#include <cstdio>
#include <chrono>
#include <stdexcept>

Pipeline::Pipeline(const Config& cfg) : cfg_(cfg) {
    cam_.reset(new CameraV4L2(cfg_.device, cfg_.cam_w, cfg_.cam_h));
    dec_.reset(new MppDecoder()); 
    npu_.reset(new RknnEngine(cfg_.model.c_str()));

    // Zero-copy:
    if (npu_->setup_io() != 0) {
        throw std::runtime_error("rknn setup_io failed");
    }
    npu_input_fd_ = npu_->input_fd();

    cam_->start();
}

int Pipeline::run() {
    int ok = 0;
    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg_.frames; ++i) {
        uint32_t sz = 0;
        int idx = cam_->dequeue(&sz);
        if (idx < 0) break;

        const uint8_t* jpeg = static_cast<const uint8_t*>(cam_->buffer(idx));
        int fd_nv12 = dec_->decode(jpeg, sz);
        cam_->requeue(idx);

	if (i < 5)
            fprintf(stderr, "pip[%d]: idx=%d sz=%u fd=%d\n", i, idx, sz, fd_nv12);

        if (fd_nv12 < 0) continue;

        // RGA
        if (!rga_.resize(fd_nv12, cfg_.cam_w, cfg_.cam_h,
                         npu_->input_fd(), cfg_.model_w, cfg_.model_h)) {
            fprintf(stderr, "RGA resize failed\n");
            continue;
        }

        if (npu_->run() != 0) { fprintf(stderr, "rknn_run failed\n"); continue; }
        ++ok;
    }

    double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    printf("Processed %d frames in %.3f s -> %.1f FPS\n", ok, sec,
           sec > 0 ? ok / sec : 0.0);
    return ok;
}
