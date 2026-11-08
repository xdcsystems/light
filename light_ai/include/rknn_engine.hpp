#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include "rknn_api.h"

class RknnEngine
{
public:
    explicit RknnEngine( const char* model_path );
    ~RknnEngine();

    // Non-copyable by design:
    // rknn_context and the io-mem pointers are runtime-owned handles;
    // a copy would destroy them twice and invalidate the Pipeline pool slot pointers.
    // The pool reserves vector capacity up front and never reallocates;
    // these deletions turn any violation of that contract into a compile-time error
    // instead of a silent NPU handle corruption.
    RknnEngine( const RknnEngine& ) = delete;
    RknnEngine& operator=( const RknnEngine& ) = delete;
    // Move-only: vector<RknnEngine> requires MoveInsertable for reserve().
    // The move transfers the rknn handles; the moved-from engine becomes
    // an inert shell (ctx_ == 0 makes the dtor and every method a no-op),
    // so a stray move can never double-destroy an NPU context.
    RknnEngine( RknnEngine&& o ) noexcept
      : ctx_( o.ctx_ )
      , in_mem_( o.in_mem_ )
      , out_mems_( std::move( o.out_mems_ ) )
      , in_attr_( o.in_attr_ )
      , out_attrs_( std::move( o.out_attrs_ ) )
    {
        o.ctx_ = 0;
        o.in_mem_ = nullptr;
        o.out_mems_.clear();
    }

    RknnEngine& operator=( RknnEngine&& ) = delete;

    int setup_io();
    int run() const;

    // Pin inference to NPU core(s) via rknn_set_core_mask.
    // mask: 0 = AUTO (do not call), 1/2/4 = core 0/1/2 (the per-core
    // flags of rknn_api.h, used by the pool), 3 = cores 0+1, 7 = 0+1+2.
    int set_core_mask( unsigned mask ) const;

    void* input_data() const { return in_mem_ ? in_mem_->virt_addr : nullptr; }
    int input_fd() const { return in_mem_ ? in_mem_->fd : -1; }
    uint32_t input_size() const { return in_mem_ ? in_mem_->size : 0; }

    const rknn_tensor_attr& input_attr() const { return in_attr_; }
    const std::vector<rknn_tensor_attr>& output_attrs() const { return out_attrs_; }
    // Output i as raw model-type bytes (int8_t here is just a byte view;
    // the real type is out_attrs_[i].type -- the model yields fp16).
    const int8_t* output_data( size_t i ) const
    {
        return ( i < out_mems_.size() && out_mems_[i] ) ? static_cast<const int8_t*>( out_mems_[i]->virt_addr )
                                                        : nullptr;
    }
    uint32_t output_size( size_t i ) const {
        return ( i < out_mems_.size() && out_mems_[i] ) ? out_mems_[i]->size : 0;
    }

private:
    rknn_context ctx_ = 0;
    rknn_tensor_mem* in_mem_ = nullptr;
    std::vector<rknn_tensor_mem*> out_mems_;
    rknn_tensor_attr in_attr_ {};
    std::vector<rknn_tensor_attr> out_attrs_;
};
