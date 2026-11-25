// RKNN Re-ID embedding worker: wraps one RknnEngine (with 4 zero-copy input slots
// and native FP32/FP16/INT8 output support) and a BasicReidQueue background thread.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "core/frame_view.hpp"    // TensorRef
#include "core/global_track.hpp"  // ReidJob, ReidResult, reid_cfg
#include "core/log.hpp"
#include "npu/reid_queue.hpp"
#include "npu/rknn_engine.hpp"

class ReidWorker
{
    struct EngineRunner
    {
        RknnEngine* engine = nullptr;
        std::atomic<uint64_t>* max_ns = nullptr;
        bool operator()( const ReidJob& job, ReidResult& res ) const noexcept;
    };

    struct BigCorePin
    {
        const Log* log = nullptr;
        void operator()() const noexcept;
    };

public:
    ReidWorker( const char* model_path, int pool_size, int infer_core, const Log& log );
    ~ReidWorker() = default;
    ReidWorker( const ReidWorker& ) = delete;
    ReidWorker& operator=( const ReidWorker& ) = delete;
    ReidWorker( ReidWorker&& ) = delete;
    ReidWorker& operator=( ReidWorker&& ) = delete;

    int input_width() const noexcept { return engine_.input_width(); }
    int input_height() const noexcept { return engine_.input_height(); }

    TensorRef input_tensor( int slot ) const noexcept { return engine_.input_tensor( slot ); }
    int inflight_for( int src ) const noexcept { return queue_.inflight_for( src ); }
    int free_slot_count() const noexcept { return queue_.free_slot_count(); }
    int try_acquire_slot() noexcept { return queue_.try_acquire_slot(); }
    bool submit( const ReidJob& job ) noexcept { return queue_.submit( job ); }
    bool poll_result( ReidResult& out ) noexcept { return queue_.poll_result( out ); }
    double take_max_ms() noexcept;

private:
    RknnEngine engine_;
    std::atomic<uint64_t> max_ns_ { 0 };
    BasicReidQueue<EngineRunner, BigCorePin> queue_;
};
