#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "pipeline.hpp"
#include "coco_names.h"
#include "yolo_postprocess.hpp"
#include <sys/stat.h>  // stat(): startup node fallback

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

// Colors shared by the GPU and CPU drawing paths (owner's pick).
inline void det_color(int cls, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (cls == 0)       { r = 255; g = 64;  b = 64;  }  // person
    else if (cls == 56) { r = 64;  g = 255; b = 64;  }  // chair
    else                { r = 255; g = 255; b = 0;   }  // others
}

// M3.7 CPU baseline: 1-px outlines straight into the NPU tensor (RGB888).
// The caller pre-maps boxes to tensor coordinates — the same geometry as
// the GPU path; colors come from the box itself. Used when the GPU drawer
// is unavailable, or forced by --cpu-draw (A/B baseline).
void draw_boxes_cpu(uint8_t* rgb, int w, int h, int stride,
                    const std::vector<GpuBox>& boxes) {
    auto clamp = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    for (const auto& b : boxes) {
        const int x0 = clamp(static_cast<int>(b.x0), w - 1);
        const int y0 = clamp(static_cast<int>(b.y0), h - 1);
        const int x1 = clamp(static_cast<int>(b.x1), w - 1);
        const int y1 = clamp(static_cast<int>(b.y1), h - 1);
        auto put = [&](int x, int y) {
            uint8_t* px = rgb + static_cast<size_t>(y) * stride + x * 3;
            px[0] = b.r; px[1] = b.g; px[2] = b.b;
        };
        for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
        for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
    }
}

} // namespace

Pipeline::Pipeline(const Config& cfg)
    : cfg_(cfg),
      cam_(cfg_.cam_w, cfg_.cam_h),
      dec_(cfg_.cam_w, cfg_.cam_h),
      npu_(cfg_.model.c_str()) {
    cam_node_ = cfg_.device;
    // Startup fallback for a re-enumerated camera: the configured node
    // may be gone after a replug (verified live: video20 -> video21)
    // while the camera itself is alive. ENOENT-only: if the node exists
    // but setup fails (busy, permissions, wrong node type), the error
    // stays fatal — a silently substituted source is worse than a loud
    // one. find_usb_capture_node() skips a missing preferred path on
    // its own (stat) and trial-setups the candidates, so fake nodes
    // (metadata-only) are filtered there.
    if (!cam_node_.empty()) {
        struct stat st{};
        if (stat(cam_node_.c_str(), &st) != 0) {
            std::cerr << "camera: configured node " << cam_node_
                      << " not present, falling back to USB discovery\n";
            std::string found;
            if (find_usb_capture_node(cam_, cam_node_, found) != 0)
                throw std::runtime_error("camera: configured node " +
                    cam_node_ + " is missing and no USB capture node found");
            std::cout << "camera: discovery adopted " << found << '\n';
            cam_node_ = found;
        }
    }

    std::string cam_why;
    if (cam_.setup_device(cam_node_, &cam_why) != 0) {
        throw std::runtime_error("camera: " + cam_why);
    }
    if (npu_.setup_io() != 0) {
        throw std::runtime_error("rknn setup_io failed");
    }
    // M2.5: optional NPU core pinning. Default (0) keeps the runtime's
    // AUTO behaviour exactly as before — the A/B baseline is untouched.
    if (cfg_.core != 0) {
        if (npu_.set_core_mask(static_cast<unsigned>(cfg_.core)) != 0) {
            throw std::runtime_error("rknn set_core_mask failed");
        }
        std::cout << "npu: core mask 0x" << std::hex << cfg_.core
                  << std::dec << "\n";
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

    if (cfg_.net) {
        // Hard confirmation of tensor contiguity: if size_with_stride equals
        // w*h*3, the buffer is exactly the PPM dump layout (stride = w*3).
        std::cout << "net: tensor stride check: size_with_stride="
                  << ia.size_with_stride << " vs "
                  << model_w_ * model_h_ * 3 << "\n";
        if (cfg_.net_h264) {
            // M3.5 pull: empty net_host = TCP pull server (listener
            // on net_port, the viewer connects to the board's LAN
            // IP); a non-empty net_host keeps the legacy UDP push
            // to that destination.
            // M3.5: the h264 encoder consumes the NPU input tensor AFTER
            // box drawing, at the NATIVE letterbox-band size (no upscale):
            // w = model_w, h = cam_h * (model_w / cam_w) — for the default
            // 1280x720 camera and a 640x640 tensor that is 640x360 (16:9).
            // The encoder derives the same band geometry from these numbers.
            const float s = static_cast<float>(model_w_) / cfg_.cam_w;
            const int band_h = static_cast<int>(cfg_.cam_h * s + 0.5f);
            enc_.emplace(model_w_, band_h, cfg_.net_fps,
                         cfg_.net_port, cfg_.net_host, cfg_.net_bitrate,
                         model_w_, model_h_);
            if (cfg_.net_host.empty()) {
                std::cout << "net: h264 PULL server on port "
                          << cfg_.net_port << ", " << cfg_.net_bitrate
                          << " kbps (viewer: ffplay tcp://<board-ip>:"
                          << cfg_.net_port << ")\n";
            } else {
                std::cout << "net: h264 preview -> " << cfg_.net_host
                          << ':' << cfg_.net_port << ", "
                          << cfg_.net_bitrate << " kbps\n";
            }
        } else {
            preview_.emplace(
                model_w_, model_h_, model_w_ * 3, cfg_.net_fps, cfg_.net_port);
            std::cout << "net: preview server on port " << cfg_.net_port
                      << "\n";
        }
    }

    // M3.7: GPU box drawing (Mali G610, EGL/GBM) straight into the NPU
    // tensor. Init once, only with a preview sink active. On failure the
    // CPU path (net_preview::draw_boxes_rgb24) stays as the fallback.
    // --cpu-draw forces the CPU path (A/B baseline).
    if ((preview_ || enc_) && !cfg_.cpu_draw) {
        drawer_.emplace();
        if (!drawer_->init() ||
            !drawer_->attachTensor(npu_.input_fd(), model_w_, model_h_,
                                   static_cast<uint32_t>(model_w_) * 3)) {
            std::cerr << "pipeline: gpu drawer unavailable, RGA fallback\n";
            drawer_.reset();
        } else {
            std::cout << "net: gpu box drawing active (Mali G610, EGL/GBM)\n";
        }
    }

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

    if (cfg_.cal_frames > 0) {
        std::error_code ec;
        std::filesystem::create_directories("dataset", ec);
        if (ec) {
            throw std::runtime_error("pipeline: cannot create dataset/: "
                                     + ec.message());
        }
    }
    cam_.start();
}

int Pipeline::run() {
    int ok = 0;
    const auto t_start = clk::now();

    // Disconnect policy: ENODEV/EIO from DQBUF mean the device is gone
    // (unplug, USB hiccup) — worth a few timed reconnect attempts; any
    // other errno is a hard error and ends the run with exit code 1
    // (run() returns -1, main() maps it to 1). Exhausted reconnect
    // attempts end the same way; a silent exit 0 on camera death is the
    // bug this code removes. A recovery attempt consumes one frame of
    // the --frames budget and contributes nothing to the timing stats.
    // 20 attempts: observed USB re-enumeration on this board can take
    // longer than the original 10x1s window allowed.
    constexpr int kCamReconnectMax = 20;
    constexpr auto kCamReconnectPause = std::chrono::seconds(1);
    constexpr int kWarmupFrames = 3;
    int reconnects = 0;
    int warmup_left = 0;   // publish-discards after reconnect

    smoother_.set_log(cfg_.box_log);
    for (int i = 0; i < cfg_.frames; ++i) {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        int cam_err = 0;
        const int idx = cam_.dequeue(&sz, &cam_err);
        if (idx < 0) {
            // Classify the failure before acting on it.
            const bool recoverable = (cam_err == ENODEV || cam_err == EIO);
            std::cerr << "camera: dequeue failed: " << strerror(cam_err)
                      << (recoverable ? " (recoverable)" : " (fatal)")
                      << '\n';
            if (!recoverable) return -1;
            bool back = false;
            for (int a = 1; a <= kCamReconnectMax; ++a) {
                std::this_thread::sleep_for(kCamReconnectPause);
                std::string node;
                if (find_usb_capture_node(cam_, cam_node_, node) != 0)
                    continue;
                std::string why;
                if (cam_.reconnect(node, &why) == 0) {
                    if (node != cam_node_) {
                        std::cout << "camera: re-enumerated node adopted: "
                                  << cam_node_ << " -> " << node << '\n';
                        cam_node_ = node;
                    }
                    ++reconnects;
                    warmup_left = kWarmupFrames;
                    std::cout << "camera: reconnected on attempt " << a
                              << '\n';
                    back = true;
                    break;
                }
                std::cerr << "camera: reconnect on " << node
                          << " failed: " << why << '\n';
            }
            if (!back) {
                std::cerr << "camera: reconnect failed after "
                          << kCamReconnectMax << " attempts, giving up\n";
                return -1;
            }
            continue;
        }
        const auto t1 = clk::now();

        // Zero-size packet guard: a dying/reconnecting camera can hand
        // out a buffer with bytesused == 0. Feeding it to the decoder
        // produces "BAD INPUT ok=1 ... size=0" — MPP reports success on
        // an EMPTY packet, and the buffer behind the returned fd is
        // stale/uninitialized memory. RGA then stretches that garbage
        // into the NPU tensor, YOLO hallucinates on it and the stream
        // shows phantom boxes (observed right after a replug). Skip the
        // decode, keep the buffer cycle intact (requeue still runs).
        int fd_src = -1;
        if (sz > 0) {
            fd_src = dec_.decode(static_cast<const uint8_t*>(cam_.buffer(idx)), sz);
        } else {
            std::cerr << "camera: zero-size frame skipped\n";
        }
        cam_.requeue(idx);
        const auto t2 = clk::now();

        // A frame that failed to decode contributes neither to ok nor to the
        // timing statistics — otherwise rare errors would silently skew averages.
        if (fd_src < 0) continue;

        // Warm-up discard: even non-empty first packets after STREAMON
        // start mid-stream, so the first decoded frame(s) after a
        // reconnect can be partial garbage. They ARE decoded (flushing
        // the decoder is the point) but never inferred or published —
        // a two-frame gap beats phantoms on a live stream. The counter
        // counts VALID decodes only: garbage frames (fd < 0) do not
        // consume it, so publishing resumes on real data.
        if (warmup_left > 0) {
            --warmup_left;
            if (warmup_left == 0)
                std::cout << "camera: warm-up done, publishing resumed\n";
            continue;
        }

        const bool rga_ok = rga_.resize(fd_src, cfg_.cam_w, cfg_.cam_h,
                                        npu_.input_fd(), model_w_, model_h_,
                                        aux_fd_, cfg_.cam_w / 2, cfg_.cam_h / 2);

        const auto t3 = clk::now();

        // Calibration dump: P6 PPM of the model input. The only diagnostic
        // CPU pixel read. --dump-cal writes a single /tmp/cal.ppm; --cal N
        // saves up to N frames (one every --cal-interval) into dataset/.
        const bool cal_wanted = cfg_.dump_cal ||
            (cfg_.cal_frames > 0 && cal_saved_ < cfg_.cal_frames);
        const bool cal_due = !cal_done_ || (i - last_cal_ >= cfg_.cal_interval);
        if (rga_ok && cal_wanted && cal_due) {
            const size_t bytes = static_cast<size_t>(model_w_) * model_h_ * 3;
            std::string path = "/tmp/cal.ppm";
            if (!cfg_.dump_cal) {
                std::ostringstream name;
                name << "dataset/cal_" << std::setw(4) << std::setfill('0')
                     << cal_saved_ << ".ppm";
                path = name.str();
            }
            std::ofstream out(path, std::ios::binary);
            if (out && npu_.input_size() >= bytes) {
                out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
                out.write(reinterpret_cast<const char*>(npu_.input_data()),
                          static_cast<std::streamsize>(bytes));
                if (cfg_.dump_cal) {
                    std::cout << "calibration dump: " << path << " ("
                              << model_w_ << 'x' << model_h_ << ")\n";
                }
                ++cal_saved_;
                last_cal_ = i;
                cal_done_ = true;
            } else {
                std::cerr << "calibration dump failed\n";
                cal_done_ = true;   // do not retry-spam on a broken sink
            }
        }
        const auto t3b = clk::now();   // cal-dump file I/O is not infer

        const bool npu_ok = rga_ok && (npu_.run() == 0);
        const auto t4 = clk::now();

        if (npu_ok) {
            // Letterbox geometry from the single source of truth: RgaResizer
            // stored it during the last resize.
            const auto dets_raw = yolo_postprocess(npu_, cfg_.cam_w,
                                               cfg_.cam_h,
                                               model_w_, model_h_,
                                               static_cast<float>(rga_.lb_top()),
                                               static_cast<float>(rga_.lb_h()),
                                               cfg_.conf_thr, cfg_.iou_thr);
            // M3.10: EMA smoothing on scene-coord boxes; the raw list
            // is kept only for the --box-log jitter A/B CSV.
            const auto dets = smoother_.update(dets_raw);
            smoother_.log_frame(i, dets_raw, dets);
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
            if (preview_ || enc_) {
                // Draw after run(): RGA overwrites the band next frame, so
                // boxes never leak into inference. Cal dumps are also clean
                // (they happen before run()).
                // M3.7: boxes are mapped to tensor coordinates ONCE for
                // both drawing paths — a single source of geometry and
                // colors (det_color); the GPU draws them in one call,
                // the CPU fallback rasterizes the same list.
                const float s = static_cast<float>(model_w_) / cfg_.cam_w;
                const float lb_top = static_cast<float>(rga_.lb_top());
                std::vector<GpuBox> boxes;
                boxes.reserve(dets.size());
                for (const auto& d : dets) {
                    GpuBox b;
                    b.x0 = d.x * s;
                    b.y0 = d.y * s + lb_top;
                    b.x1 = (d.x + d.w) * s;
                    b.y1 = (d.y + d.h) * s + lb_top;
                    if (b.x1 - b.x0 < 2.0f || b.y1 - b.y0 < 2.0f) continue;
                    det_color(d.cls, b.r, b.g, b.b);
                    b.a = 255;
                    boxes.push_back(b);
                }
                if (drawer_) {
                    drawer_->draw(boxes);
                    // publish() reads the tensor right away (CPU in the NUT
                    // path, RGA in the h264 path): GPU writes must land first.
                    drawer_->syncCpu();
                } else {
                    // Fallback: CPU drawing. The RGA imrectangle path is
                    // gone for good: RGA fill on RGB888 is not routable on
                    // RK3588 (RGA3 cores cannot fill; RGA2E policy rejects
                    // rgb888 — dmesg "invalid function policy").
                    // --cpu-draw forces this path even when the GPU drawer
                    // is up (A/B baseline).
                    draw_boxes_cpu(static_cast<uint8_t*>(npu_.input_data()),
                                   model_w_, model_h_, model_w_ * 3, boxes);
                }
                // M3.5: NetPreview publishes the 640x640 model tensor
                // (NUT path). NetEncoder now publishes the same tensor:
                // the GPU/CPU drawing above has landed and is synced, so
                // the RGA band crop carries the boxes into the stream.
                if (preview_) {
                    preview_->publish(
                        static_cast<const uint8_t*>(npu_.input_data()));
                }
                if (enc_) {
                    // Tensor path: the encoder crops the visible band out
                    // of the tensor (boxes already drawn and synced) and
                    // converts it to NV12 1:1 — no upscale.
                    enc_->publish(npu_.input_fd());
                }
            }
            ++ok;
        }
        g_t.add(ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3b, t4));
    }

    const double wall = std::chrono::duration<double>(clk::now() - t_start).count();
    if (reconnects > 0) {
        std::cout << "camera disconnects recovered: " << reconnects << '\n';
    }
    g_t.report("TOTAL ");
    std::cout << "Processed " << ok << " frames in "
              << std::fixed << std::setprecision(3) << wall << " s -> "
              << (wall > 0 ? ok / wall : 0.0) << " FPS\n";
    return ok;
}
