#pragma once
#include <cstdint>
#include <vector>
#include "rknn_api.h"

class RknnEngine {
public:
    explicit RknnEngine(const char* model_path);
    ~RknnEngine();

    int setup_io();
    int run();

    // M2.5: pin inference to NPU core(s) via rknn_set_core_mask.
    // mask: 0 = AUTO (do not call), 1 = core 0, 3 = cores 0+1, 7 = 0+1+2.
    int set_core_mask(unsigned mask);

    void* input_data() const { return in_mem_ ? in_mem_->virt_addr : nullptr; }
    int    input_fd()  const { return in_mem_ ? in_mem_->fd : -1; }
    uint32_t input_size() const { return in_mem_ ? in_mem_->size : 0; }

    const rknn_tensor_attr& input_attr() const { return in_attr_; }
    const std::vector<rknn_tensor_attr>& output_attrs() const { return out_attrs_; }
    // Output i as raw model-type bytes (int8_t here is just a byte view;
    // the real type is out_attrs_[i].type — yolov5s_relu yields fp16).
    const int8_t* output_data(size_t i) const {
        return (i < out_mems_.size() && out_mems_[i]) ?
            static_cast<const int8_t*>(out_mems_[i]->virt_addr) : nullptr;
    }
    uint32_t output_size(size_t i) const {
        return (i < out_mems_.size() && out_mems_[i]) ? out_mems_[i]->size : 0;
    }

private:
    rknn_context ctx_ = 0;
    rknn_tensor_mem* in_mem_ = nullptr;
    std::vector<rknn_tensor_mem*> out_mems_;
    rknn_tensor_attr in_attr_{};
    std::vector<rknn_tensor_attr> out_attrs_;
};
