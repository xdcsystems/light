#include "pipeline.hpp"
#include <cstring>
#include <cstdlib>

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i+1 < argc)  cfg.device  = argv[++i];
        if (!strcmp(argv[i], "--model")  && i+1 < argc)  cfg.model   = argv[++i];
        if (!strcmp(argv[i], "--frames") && i+1 < argc)  cfg.frames  = atoi(argv[++i]);
        if (!strcmp(argv[i], "--w")      && i+1 < argc)  cfg.cam_w   = atoi(argv[++i]);
        if (!strcmp(argv[i], "--h")      && i+1 < argc)  cfg.cam_h   = atoi(argv[++i]);
        if (!strcmp(argv[i], "--mw")     && i+1 < argc)  cfg.model_w = atoi(argv[++i]);
        if (!strcmp(argv[i], "--mh")     && i+1 < argc)  cfg.model_h = atoi(argv[++i]);
    }

    try {
        Pipeline p(cfg);
        return p.run() > 0 ? 0 : 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
        return 2;
    }
}
