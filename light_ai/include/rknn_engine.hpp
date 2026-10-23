#pragma once
#include <cstdint>
#include "rknn_api.h"

class RknnEngine {
public:
    explicit RknnEngine(const char* model_path);
    ~RknnEngine();

    int setup_io();
    int run();

    void* input_data() const { return in_mem_ ? in_mem_->virt_addr : nullptr; }
    int    input_fd()  const { return in_mem_ ? in_mem_->fd : -1; }
    uint32_t input_size() const { return in_mem_ ? in_mem_->size : 0; }

    void* output_data() const { return out_mem_ ? out_mem_->virt_addr : nullptr; }
    uint32_t output_size() const { return out_mem_ ? out_mem_->size : 0; }

    const rknn_tensor_attr& input_attr() const { return in_attr_; }
    const rknn_tensor_attr& output_attr() const { return out_attr_; }

private:
    rknn_context ctx_ = 0;
    rknn_tensor_mem* in_mem_ = nullptr;
    rknn_tensor_mem* out_mem_ = nullptr;
    rknn_tensor_attr in_attr_{};
    rknn_tensor_attr out_attr_{};
};
