#include "rknn_engine.hpp"
#include <iostream>

RknnEngine::RknnEngine(const char* model_path) {
    std::cout << "loading model: " << model_path << '\n';

    if (rknn_init(&ctx_, const_cast<char*>(model_path), 0, 0, nullptr) != RKNN_SUCC) {
        std::cerr << "rknn_init failed for " << model_path << '\n';
        ctx_ = 0;
        return;
    }

    rknn_input_output_num io_num{};
    if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input < 1 || io_num.n_output < 1) {
        std::cerr << "rknn_query IN_OUT_NUM failed\n";
        rknn_destroy(ctx_);
        ctx_ = 0;
        return;
    }

    in_attr_ = {};
    in_attr_.index = 0;
    if (rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof(in_attr_)) != RKNN_SUCC) {
        std::cerr << "rknn_query INPUT_ATTR failed\n";
        rknn_destroy(ctx_);
        ctx_ = 0;
        return;
    }

    out_attr_ = {};
    out_attr_.index = 0;
    if (rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attr_, sizeof(out_attr_)) != RKNN_SUCC) {
        std::cerr << "rknn_query OUTPUT_ATTR failed\n";
        rknn_destroy(ctx_);
        ctx_ = 0;
    }
}

RknnEngine::~RknnEngine() {
    if (!ctx_) return;
    if (in_mem_)  rknn_destroy_mem(ctx_, in_mem_);
    if (out_mem_) rknn_destroy_mem(ctx_, out_mem_);
    rknn_destroy(ctx_);
    ctx_ = 0;
}

int RknnEngine::setup_io() {
    if (!ctx_) return -1;

    in_attr_.fmt  = RKNN_TENSOR_NHWC;
    in_attr_.type = RKNN_TENSOR_UINT8;
    in_attr_.pass_through = 0;
    in_attr_.w_stride = 0;
    in_attr_.h_stride = 0;

    const uint32_t in_size = in_attr_.size_with_stride ? in_attr_.size_with_stride
                                                       : in_attr_.size;
    in_mem_ = rknn_create_mem(ctx_, in_size);
    if (!in_mem_) {
        std::cerr << "rknn_create_mem (input) failed\n";
        return -1;
    }

    if (rknn_set_io_mem(ctx_, in_mem_, &in_attr_) != RKNN_SUCC) {
        std::cerr << "rknn_set_io_mem (input) failed\n";
        return -1;
    }

    // The output buffer is sized exactly as the model expects. For an INT8
    // output this is n_elems bytes, not n_elems * sizeof(float) — the old
    // x4 over-allocation worked only by luck of the runtime accepting it.
    const uint32_t out_size = out_attr_.size;
    out_mem_ = rknn_create_mem(ctx_, out_size);
    if (!out_mem_) {
        std::cerr << "rknn_create_mem (output) failed\n";
        return -1;
    }

    if (rknn_set_io_mem(ctx_, out_mem_, &out_attr_) != RKNN_SUCC) {
        std::cerr << "rknn_set_io_mem (output) failed\n";
        return -1;
    }

    // NHWC: dims = {N, H, W, C}; print W x H honestly.
    std::cout << "model input: " << in_attr_.dims[in_attr_.n_dims - 2] << 'x'
              << in_attr_.dims[in_attr_.n_dims - 3] << ' '
              << get_type_string(in_attr_.type) << ", output: "
              << out_attr_.n_elems << " elems\n";

    return 0;
}

int RknnEngine::run() {
    return rknn_run(ctx_, nullptr) == RKNN_SUCC ? 0 : -1;
}
