// AnalyticsStage: per-source smoothing and tracking (BoxSmoother, Tracker),
// preallocated DetList / TrajList buffers, optional Re-ID worker, and
// cross-camera GlobalTrackTable. Extracted from Pipeline.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "config/config.hpp"
#include "core/detection.hpp"
#include "core/frame_view.hpp"
#include "core/global_track.hpp"
#include "core/net_perf.hpp"
#include "core/log.hpp"
#include "core/track.hpp"
#include "core/tracker.hpp"
#include "media/rga_resizer.hpp"
#include "npu/infer_slot.hpp"
#include "npu/reid_worker.hpp"
#include "render/box_smoother.hpp"

class AnalyticsStage
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources
    };

private:
    struct SourceAnalytics
    {
        BoxSmoother smoother {};
        Tracker tracker {};
        uint32_t seen_attach_seq = 0;
        int frames = 0;  // frames dispatched from this source (cal/box-log index)
    };

public:
    explicit AnalyticsStage( const Log& log );
    ~AnalyticsStage() = default;
    AnalyticsStage( const AnalyticsStage& ) = delete;
    AnalyticsStage& operator=( const AnalyticsStage& ) = delete;
    AnalyticsStage( AnalyticsStage&& ) = delete;
    AnalyticsStage& operator=( AnalyticsStage&& ) = delete;

    // Configures model dimensions, global track similarity threshold, and
    // starts the optional Re-ID worker when enabled and within RGA limits.
    void init( const InferConfig& cfg, int model_w, int model_h, int pool_size );

    // Opens per-source --box-log files at run() start.
    void init_box_logs( const std::string& box_log, int nsrc );

    // Updates seen_attach_seq for src; returns true when a new attach was observed.
    bool sync_attach_seq( int src, uint32_t seq ) noexcept;

    int frame_count( int src ) const noexcept { return an_[static_cast<size_t>( src )].frames; }
    int next_frame_index( int src ) noexcept { return an_[static_cast<size_t>( src )].frames++; }
    bool has_reid() const noexcept { return reid_worker_.has_value(); }
    ReidPerfWindow take_reid_perf() noexcept;

    // Runs BoxSmoother, Tracker, and optional Re-ID stage for one NPU result.
    void process( const InferResult& res, const FrameView& view, const RgaResizer& rga ) noexcept;

    void reset_source_analytics( int src, int64_t ts_us ) noexcept;

    const DetList& dets() const noexcept { return dets_; }
    const TrajList& trajs() const noexcept { return trajs_; }
    const Tracker& tracker( int src ) const noexcept { return an_[static_cast<size_t>( src )].tracker; }

private:
    void start_reid( const InferConfig& cfg, int pool_size );
    void drain_reid_results() noexcept;
    void run_reid_stage( const FrameView& view, bool rga_ok, const RgaResizer& rga ) noexcept;

private:
    const Log& log_;
    std::array<SourceAnalytics, kMaxSources> an_ {};
    DetList dets_ {};
    TrajList trajs_ {};
    std::optional<ReidWorker> reid_worker_ {};
    GlobalTrackTable global_tracks_ {};
    uint64_t reid_crop_fail_ = 0;
    ReidPerfWindow reid_perf_ {};
    int model_w_ = 0;
    int model_h_ = 0;
    int nsrc_ = 1;
};
