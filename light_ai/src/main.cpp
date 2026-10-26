#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

#include "pipeline.hpp"

namespace {

bool parse_int(const char* s, int* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

bool parse_float(const char* s, float* out) {
    if (!s || !*s) return false;
    errno = 0;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    *out = v;
    return true;
}

// Unified value-rejection report; expected == nullptr omits the hint
// (--frames/--w/--h have no range and never printed one).
bool value_error(const char* opt, const char* val, const char* expected) {
    std::cerr << "invalid value for " << opt << ": '" << val << '\'';
    if (expected) std::cerr << " (expected " << expected << ')';
    std::cerr << '\n';
    return false;
}

// One entry per option; flags carry needs_value = false and never touch
// val (it arrives as nullptr). Adding an option is a single table row,
// not another branch. std::map, not unordered_map: the table is consulted
// ~14 times per run, lookup speed is irrelevant, while sorted iteration
// gives --help a stable listing for free.
struct Option {
    bool needs_value;
    const char* desc;
    bool (*apply)(Config& cfg, const char* val);
};

// Model input dimensions are read from the model itself, so --mw/--mh are
// intentionally gone: they could only desynchronize RGA from the real tensor.
const std::map<std::string, Option> kOptions = {

    // --- flags (no value) ---
    {"--dump-cal", {false, "collect calibration dataset",
        [](Config& c, const char*) { c.dump_cal = true; return true; }}},
    {"--net", {false, "network preview (raw rgb24 or --net-h264)",
        [](Config& c, const char*) { c.net = true; return true; }}},
    {"--net-h264", {false, "H.264/MPEG-TS preview instead of raw rgb24",
        [](Config& c, const char*) { c.net_h264 = true; return true; }}},
    // M3.7: force the CPU box-drawing path (A/B vs the GPU drawer).
    {"--cpu-draw", {false, "draw boxes on CPU (A/B vs GPU drawer)",
        [](Config& c, const char*) { c.cpu_draw = true; return true; }}},

    // --- valued options ---
    {"--device", {true, "camera device path",
        [](Config& c, const char* v) { c.device = v; return true; }}},
    {"--model", {true, "RKNN model path",
        [](Config& c, const char* v) { c.model = v; return true; }}},

    {"--conf", {true, "confidence threshold 0.0..1.0",
        [](Config& c, const char* v) {
            float f;
            if (!parse_float(v, &f) || f < 0.0f || f > 1.0f)
                return value_error("--conf", v, "0.0..1.0");
            c.conf_thr = f;
            return true;
        }}},
    {"--iou", {true, "NMS IoU threshold 0.0..1.0",
        [](Config& c, const char* v) {
            float f;
            if (!parse_float(v, &f) || f < 0.0f || f > 1.0f)
                return value_error("--iou", v, "0.0..1.0");
            c.iou_thr = f;
            return true;
        }}},

    {"--cal", {true, "calibration frames 1..10000",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n) || n < 1 || n > 10000)
                return value_error("--cal", v, "1..10000");
            c.cal_frames = n;
            return true;
        }}},
    {"--cal-interval", {true, "calibration sampling stride 1..10000",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n) || n < 1 || n > 10000)
                return value_error("--cal-interval", v, "1..10000");
            c.cal_interval = n;
            return true;
        }}},

    {"--frames", {true, "stop after N frames",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n)) return value_error("--frames", v, nullptr);
            c.frames = n;
            return true;
        }}},
    {"--w", {true, "camera width",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n)) return value_error("--w", v, nullptr);
            c.cam_w = n;
            return true;
        }}},
    {"--h", {true, "camera height",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n)) return value_error("--h", v, nullptr);
            c.cam_h = n;
            return true;
        }}},

    {"--core", {true, "NPU cores: 0 auto, 1, 3 (0+1), 7 (0+1+2)",
        [](Config& c, const char* v) {
            int n;
            // Exactly the masks set_core_mask() implements: anything else
            // silently degrades to the runtime default (auto).
            if (!parse_int(v, &n) ||
                (n != 0 && n != 1 && n != 3 && n != 7))
                return value_error("--core", v, "0|1|3|7");
            c.core = n;
            return true;
        }}},

    {"--net-port", {true, "preview port 1..65535",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n) || n < 1 || n > 65535)
                return value_error("--net-port", v, "1..65535");
            c.net_port = n;
            return true;
        }}},
    {"--net-fps", {true, "preview stream fps 1..120",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n) || n < 1 || n > 120)
                return value_error("--net-fps", v, "1..120");
            c.net_fps = n;
            return true;
        }}},
    {"--net-bitrate", {true, "preview bitrate 1000..100000 kbps",
        [](Config& c, const char* v) {
            int n;
            if (!parse_int(v, &n) || n < 1000 || n > 100000)
                return value_error("--net-bitrate", v, "1000..100000 kbps");
            c.net_bitrate = n;
            return true;
        }}},
    {"--net-host", {true, "optional UDP destination (h264 push); unset = TCP pull",
        [](Config& c, const char* v) { c.net_host = v; return true; }}},
    {"--box-log", {true, "per-frame box CSV for jitter A/B (raw vs smoothed)",
        [](Config& c, const char* v) { c.box_log = v; return true; }}},
};

void usage() {
    std::cout << "usage: light_ai [options]\noptions:\n";
    for (const auto& kv : kOptions) {
        const Option& o = kv.second;
        std::cout << "  " << kv.first
                  << (o.needs_value ? " <value>" : "")
                  << " - " << o.desc << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const char* opt = argv[i];
        if (!std::strcmp(opt, "-h") || !std::strcmp(opt, "--help")) {
            usage();
            return 0;
        }
        const auto it = kOptions.find(opt);
        if (it == kOptions.end()) {
            std::cerr << "unknown option: " << opt << '\n';
            return 2;
        }
        const char* val = nullptr;
        if (it->second.needs_value) {
            if (i + 1 >= argc) {
                std::cerr << "missing value for option: " << opt << '\n';
                return 2;
            }
            val = argv[++i];
        }
        if (!it->second.apply(cfg, val)) return 2;
    }

    // --net-h264 is a variant of the preview subsystem, not an independent
    // switch: the whole preview setup (encoder, gpu drawer, diagnostics)
    // lives under cfg_.net, so --net-h264 without --net silently runs a
    // stream-less pipeline. Fail loudly instead (checked here,
    // post-parse, because --net may come after --net-h264 on the
    // command line).
    if (cfg.net_h264 && !cfg.net) {
        std::cerr << "--net-h264: preview is off, nothing to stream.\n"
                     "  --net-h264 selects the transport of the --net "
                     "preview subsystem (MPEG-TS over TCP pull, or UDP\n"
                     "  push with --net-host); add --net to enable the "
                     "preview itself, e.g.:\n"
                     "    light_ai --net --net-h264\n";
        return 2;
    }

    try {
        Pipeline p(cfg);
        return p.run() > 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 2;
    }
}
