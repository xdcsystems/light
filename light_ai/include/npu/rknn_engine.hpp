#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <rknn_api.h>

#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "core/non_copyable.hpp"
#include "core/status.hpp"

class RknnEngine : private NonCopyable<RknnEngine>
{
public:
    // log: the diagnostics sink (the model / attribute lines, the io-mem and core-mask failures).
    RknnEngine( const char* model_path, const Log& log );
    ~RknnEngine();

    // Non-copyable by design (the NonCopyable base):
    // rknn_context and the io-mem pointers are runtime-owned handles;
    // a copy would destroy them twice and invalidate the InferencePool engine references.
    // The pool reserves vector capacity up front and never reallocates;
    // the missing copy turns any violation of that contract into a compile-time error
    // instead of a silent NPU handle corruption.
    // Move-only: vector<RknnEngine> requires MoveInsertable for reserve().
    // The move transfers the rknn handles; the moved-from engine becomes
    // an inert shell (ctx_ == 0 makes the dtor and every method a no-op),
    // so a stray move can never double-destroy an NPU context.
    RknnEngine( RknnEngine&& o ) noexcept
      : ctx_( o.ctx_ )
      , in_mems_( std::move( o.in_mems_ ) )
      , in_dma_( std::move( o.in_dma_ ) )
      , out_mems_( std::move( o.out_mems_ ) )
      , in_attr_( o.in_attr_ )
      , out_attrs_( std::move( o.out_attrs_ ) )
      , in_w_( o.in_w_ )
      , in_h_( o.in_h_ )
      , in_c_( o.in_c_ )
      , in_stride_bytes_( o.in_stride_bytes_ )
      , log_( o.log_ )
    {
        o.ctx_ = 0;
        o.in_mems_.clear();
        o.in_dma_.clear();
        o.out_mems_.clear();
        o.in_w_ = 0;
        o.in_h_ = 0;
        o.in_c_ = 0;
        o.in_stride_bytes_ = 0;
    }

    RknnEngine& operator=( RknnEngine&& ) = delete;

    Status setup_io( bool keep_native_outputs = false, int input_slots = 1 );
    Status setup_io_shared( const TensorSet& shared_inputs, bool keep_native_outputs = false );
    Status bind_input_slot( int slot ) noexcept;
    void sync_output( size_t i ) const noexcept;
    Status run() const;

    // Pin inference to NPU core(s) via rknn_set_core_mask. mask: an rknn_core_mask
    // value -- RKNN_NPU_CORE_AUTO (0) keeps the runtime default (no call, ok);
    // RKNN_NPU_CORE_0 / _1 / _2 (1/2/4, the pool's per-core pins), _0_1 (3) and
    // _0_1_2 (7) are passed on (Failed with the SDK code if the call fails);
    // any other value: Invalid "invalid core mask", no call.
    Status set_core_mask( unsigned mask ) const;

    void* input_data() const { return ( !in_mems_.empty() && in_mems_[0] ) ? in_mems_[0]->virt_addr : nullptr; }
    int input_fd() const { return ( !in_mems_.empty() && in_mems_[0] ) ? in_mems_[0]->fd : -1; }
    uint32_t input_size() const { return ( !in_mems_.empty() && in_mems_[0] ) ? in_mems_[0]->size : 0; }
    TensorRef input_tensor( int slot ) const noexcept;
    uint32_t input_stride_bytes() const noexcept { return in_stride_bytes_; }

    const rknn_tensor_attr& input_attr() const { return in_attr_; }
    // Model input geometry -- parsed from the native tensor layout (NHWC or NCHW);
    // 0 when the tensor is not 4-D.
    int input_width() const { return in_w_; }
    int input_height() const { return in_h_; }
    const std::vector<rknn_tensor_attr>& output_attrs() const { return out_attrs_; }
    // Output i as raw model-type bytes (int8_t here is just a byte view;
    // the real type is out_attrs_[i].type -- the model yields fp16).
    const int8_t* output_data( size_t i ) const
    {
        return ( i < out_mems_.size() && out_mems_[i] ) ? static_cast<const int8_t*>( out_mems_[i]->virt_addr )
                                                        : nullptr;
    }

private:
    struct Dma32Buf
    {
        int fd = -1;
        void* ptr = nullptr;
        size_t size = 0;
    };

private:
    void parse_input_dims();
    void release_io();
    Status alloc_input_slot( int slot, uint32_t in_size, bool& warned_fallback );
    // The input row width in pixels (queried w_stride or the tight width), or
    // Unsupported when it is not RGB888 rows RGA can read.
    Status input_row( int& row_px ) const;
    Status prepare_input_attr( uint32_t& out_in_size, uint32_t& out_row_bytes );
    Status setup_outputs( bool keep_native_outputs );
    Status finish_setup_io( bool keep_native_outputs, uint32_t row_bytes );

private:
    rknn_context ctx_ = 0;
    std::vector<rknn_tensor_mem*> in_mems_;
    std::vector<Dma32Buf> in_dma_;
    std::vector<rknn_tensor_mem*> out_mems_;
    rknn_tensor_attr in_attr_ {};
    std::vector<rknn_tensor_attr> out_attrs_;
    int in_w_ = 0;
    int in_h_ = 0;
    int in_c_ = 0;
    uint32_t in_stride_bytes_ = 0;
    const Log& log_;  // owned by main(); outlives every engine
};
