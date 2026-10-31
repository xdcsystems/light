// tools/bench_npu_pool.cpp — pure-infer pool probe: one model, up to 3
// RKNN contexts, one NPU core each (explicit RKNN_NPU_CORE_x pinning).
// Measures: (1) single-context serial baseline, (2) 3-worker pool
// throughput. Disposable probe: tools/ is exempt from the statics rule;
// iostream and no-heap rules still apply.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>

#include <rknn_api.h>

namespace {

constexpr int kPoolSize = 3;
constexpr int kOutMax = 4;

struct IoMem {
    rknn_tensor_mem* mem = nullptr;
    void* map = nullptr;
    size_t size = 0;
    int fd = -1;
};

bool sync_dma(int fd, uint64_t flags)
{
    dma_buf_sync s {};
    s.flags = flags;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0;
}

// Caller owns the reset: fills the IoMem clean.
void reset_iomem(IoMem& m)
{
    m.mem = nullptr;
    m.map = nullptr;
    m.size = 0;
    m.fd = -1;
}

bool alloc_and_fill_input(rknn_context ctx, const rknn_tensor_attr& attr, IoMem& out)
{
    out.mem = rknn_create_mem(ctx, attr.size_with_stride);
    if (!out.mem) {
        std::cerr << "[pool] rknn_create_mem(input) failed\n";
        return false;
    }
    out.fd = out.mem->fd;
    out.size = attr.size_with_stride;
    out.map = mmap(nullptr, out.size, PROT_READ | PROT_WRITE, MAP_SHARED, out.fd, 0);
    if (out.map == MAP_FAILED) {
        out.map = nullptr;
        std::cerr << "[pool] input mmap failed\n";
        return false;
    }
    // One gray fill; bench-only bytes, not a frame path.
    if (!sync_dma(out.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW)) {
        std::cerr << "[pool] input sync START failed\n";
        return false;
    }
    std::memset(out.map, 0x80, out.size);
    if (!sync_dma(out.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW)) {
        std::cerr << "[pool] input sync END failed\n";
        return false;
    }
    munmap(out.map, out.size);
    out.map = nullptr;
    return true;
}

struct Worker {
    rknn_context ctx = 0;
    rknn_tensor_attr in_attr {};
    rknn_tensor_attr out_attr[kOutMax];
    IoMem in {};
    IoMem out[kOutMax];
    int out_count = 0;
};

bool init_worker(Worker& w, const std::string& model, rknn_context* share_from)
{
    int ret;
    if (share_from) {
        // Weight sharing: the duped contexts alias the loaded weights.
        // If the board header lacks rknn_dup_context, replace with
        // rknn_init per worker (3x weight memory, still correct).
        ret = rknn_dup_context(share_from, &w.ctx);
        if (ret != RKNN_SUCC) {
            std::cerr << "[pool] rknn_dup_context failed: " << ret << '\n';
            return false;
        }
    } else {
        ret = rknn_init(&w.ctx, const_cast<char*>(model.c_str()),
                        0, 0, nullptr);
        if (ret != RKNN_SUCC) {
            std::cerr << "[pool] rknn_init failed: " << ret << '\n';
            return false;
        }
    }

    w.in_attr.index = 0;
    ret = rknn_query(w.ctx, RKNN_QUERY_INPUT_ATTR, &w.in_attr, sizeof(w.in_attr));
    if (ret != RKNN_SUCC) {
        std::cerr << "[pool] input query failed: " << ret << '\n';
        return false;
    }
    if (!alloc_and_fill_input(w.ctx, w.in_attr, w.in))
        return false;
    ret = rknn_set_io_mem(w.ctx, w.in.mem, &w.in_attr);
    if (ret != RKNN_SUCC) {
        std::cerr << "[pool] set_io_mem(input) failed: " << ret << '\n';
        return false;
    }

    rknn_input_output_num io_num {};
    ret = rknn_query(w.ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        std::cerr << "[pool] in/out num query failed: " << ret << '\n';
        return false;
    }
    if (io_num.n_output == 0 || io_num.n_output > kOutMax) {
        std::cerr << "[pool] unexpected output count: " << io_num.n_output << '\n';
        return false;
    }
    const uint32_t n_out = io_num.n_output;
    w.out_count = static_cast<int>(n_out);
    for (uint32_t i = 0; i < n_out; ++i) {
        w.out_attr[i].index = i;
        ret = rknn_query(w.ctx, RKNN_QUERY_OUTPUT_ATTR, &w.out_attr[i],
                         sizeof(w.out_attr[i]));
        if (ret != RKNN_SUCC) {
            std::cerr << "[pool] output query failed: " << ret << '\n';
            return false;
        }
        w.out[i].mem = rknn_create_mem(w.ctx, w.out_attr[i].size);
        if (!w.out[i].mem) {
            std::cerr << "[pool] rknn_create_mem(output) failed\n";
            return false;
        }
        ret = rknn_set_io_mem(w.ctx, w.out[i].mem, &w.out_attr[i]);
        if (ret != RKNN_SUCC) {
            std::cerr << "[pool] set_io_mem(output) failed: " << ret << '\n';
            return false;
        }
    }
    return true;
}

void teardown_worker(Worker& w)
{
    for (int i = 0; i < kOutMax; ++i) {
        if (w.out[i].mem) {
            rknn_destroy_mem(w.ctx, w.out[i].mem);
            reset_iomem(w.out[i]);
        }
    }
    if (w.in.mem) {
        rknn_destroy_mem(w.ctx, w.in.mem);
        reset_iomem(w.in);
    }
    if (w.ctx)
        rknn_destroy(w.ctx);
    w.ctx = 0;
}

bool pin_core(rknn_context ctx, int core)
{
    rknn_core_mask mask;
    switch (core) {
    case 0: mask = RKNN_NPU_CORE_0; break;
    case 1: mask = RKNN_NPU_CORE_1; break;
    case 2: mask = RKNN_NPU_CORE_2; break;
    default:
        std::cerr << "[pool] bad core index: " << core << '\n';
        return false;
    }
    int ret = rknn_set_core_mask(ctx, mask);
    if (ret != RKNN_SUCC) {
        std::cerr << "[pool] set_core_mask(" << core << ") failed: "
                  << ret << '\n';
        return false;
    }
    return true;
}

// Returns total wall ms, or -1 on failure.
double run_frames(rknn_context ctx, long frames, std::atomic<int>& fails)
{
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < frames; ++i) {
        int ret = rknn_run(ctx, nullptr);
        if (ret < 0) {
            ++fails;
            return -1.0;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

bool parse_long(const char* s, long& out)
{
    if (!s || *s == '\0')
        return false;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0)
        return false;
    out = v;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string model = "./misc/yolov5s_relu_int8_mmse.rknn";
    long frames = 600;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--model" || a == "-m") && i + 1 < argc) {
            model = argv[++i];
        } else if ((a == "--frames" || a == "-n") && i + 1 < argc) {
            if (!parse_long(argv[++i], frames)) {
                std::cerr << "bad --frames value\n";
                return 2;
            }
        } else {
            std::cerr << "unknown arg: " << a << '\n';
            return 2;
        }
    }

    Worker w[kPoolSize] = {};
    if (!init_worker(w[0], model, nullptr))
        return 1;
    for (int i = 1; i < kPoolSize; ++i) {
        if (!init_worker(w[i], model, &w[0].ctx))
            return 1;
    }
    for (int i = 0; i < kPoolSize; ++i) {
        if (!pin_core(w[i].ctx, i))
            return 1;
    }

    std::cout << "[pool] model: " << model
              << " input: " << w[0].in_attr.dims[1] << 'x'
              << w[0].in_attr.dims[2] << " zp=" << w[0].in_attr.zp
              << " scale=" << w[0].in_attr.scale
              << " outs=" << w[0].out_count << '\n';

    // Shared failure counter: warmup, serial phase, pool phase.
    std::atomic<int> fails { 0 };

    // Warmup: 10 frames per context, untimed.
    for (int i = 0; i < kPoolSize; ++i) {
        if (run_frames(w[i].ctx, 10, fails) < 0.0)
            return 1;
    }

    // Phase 1: serial baseline, core0 only.
    const double serial_ms = run_frames(w[0].ctx, frames, fails);
    if (serial_ms < 0.0)
        return 1;
    std::cout << "[pool] serial core0: " << frames << " frames in "
              << serial_ms << " ms -> " << serial_ms / frames
              << " ms/frame\n";

    // Phase 2: pool, one worker per core, each --frames frames.
    const auto t0 = std::chrono::steady_clock::now();
    std::thread th[kPoolSize];
    for (int i = 0; i < kPoolSize; ++i) {
        th[i] = std::thread([&w, i, frames, &fails]() {
            if (run_frames(w[i].ctx, frames, fails) < 0.0)
                return;
        });
    }
    for (int i = 0; i < kPoolSize; ++i)
        th[i].join();
    const auto t1 = std::chrono::steady_clock::now();
    if (fails.load() != 0) {
        std::cerr << "[pool] " << fails.load() << " run failures\n";
        return 1;
    }
    const double pool_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double pool_per_frame = pool_ms / (static_cast<double>(frames) * kPoolSize);
    const double serial_per_frame = serial_ms / static_cast<double>(frames);

    std::cout << "[pool] 3 workers: " << kPoolSize * frames
              << " frames in " << pool_ms << " ms -> " << pool_per_frame
              << " ms/frame effective\n";
    std::cout << "[pool] scaling: " << serial_per_frame / pool_per_frame
              << "x (serial vs pool)\n";

    for (int i = kPoolSize - 1; i >= 0; --i)
        teardown_worker(w[i]);
    return 0;
}

