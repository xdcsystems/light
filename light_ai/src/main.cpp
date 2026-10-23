#include "pipeline.hpp"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iostream>

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

// Model input dimensions are read from the model itself, so --mw/--mh are
// intentionally gone: they could only desynchronize RGA from the real tensor.
bool apply_option(Config& cfg, int argc, char** argv, int& i) {
    const char* opt = argv[i];
    // Flag without a value — must be handled before the generic
    // "option requires a value" check below.
    if (!std::strcmp(opt, "--dump-cal")) {
        cfg.dump_cal = true;
        return true;
    }

    if (i + 1 >= argc) {
        std::cerr << "missing value for option: " << opt << '\n';
        return false;
    }
    const char* val = argv[++i];

    if (!std::strcmp(opt, "--device"))      cfg.device = val;
    else if (!std::strcmp(opt, "--model"))  cfg.model  = val;
    else if (!std::strcmp(opt, "--conf") || !std::strcmp(opt, "--iou")) {
        float f = 0.0f;
        if (!parse_float(val, &f) || f < 0.0f || f > 1.0f) {
            std::cerr << "invalid value for " << opt << ": '" << val
                      << "' (expected 0.0..1.0)\n";
            return false;
        }
        if (!std::strcmp(opt, "--conf")) cfg.conf_thr = f;
        else                             cfg.iou_thr  = f;
    } else if (!std::strcmp(opt, "--cal") ||
             !std::strcmp(opt, "--cal-interval")) {
        int n = 0;
        if (!parse_int(val, &n) || n < 1 || n > 10000) {
            std::cerr << "invalid value for " << opt << ": '" << val
                      << "' (expected 1..10000)\n";
            return false;
        }
        if (!std::strcmp(opt, "--cal")) cfg.cal_frames = n;
        else                            cfg.cal_interval = n;
    } else if (!std::strcmp(opt, "--frames") ||
             !std::strcmp(opt, "--w")      ||
             !std::strcmp(opt, "--h")) {
        int n = 0;
        if (!parse_int(val, &n)) {
            std::cerr << "invalid value for " << opt << ": '" << val << "'\n";
            return false;
        }
        if (!std::strcmp(opt, "--frames")) cfg.frames = n;
        else if (!std::strcmp(opt, "--w")) cfg.cam_w  = n;
        else                               cfg.cam_h  = n;
    } else {
        std::cerr << "unknown option: " << opt << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (!apply_option(cfg, argc, argv, i)) return 2;
    }

    try {
        Pipeline p(cfg);
        return p.run() > 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 2;
    }
}
