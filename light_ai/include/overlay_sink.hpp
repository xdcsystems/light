// Overlay drawing (Mali G610 EGL/GBM or CPU fallback) and network
// preview publishing (NetEncoder H.264 / NetPreview raw NUT).
// Extracted from Pipeline to keep the composition root focused on
// source capture, NPU scheduling and per-source state.
// Works on the pool's model-input tensors through TensorSet / FrameView
// (core/frame_view.hpp): it never sees the pool, its slots or its engines.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "box_mapper.h"             // BoxMapper
#include "config/config.hpp"        // LabelLang
#include "core/box_filter.hpp"      // TrackBoxFilters, BoxFilterParams
#include "core/callout.hpp"         // CalloutHistory, CalloutList
#include "core/detection.hpp"       // DetList
#include "core/encoder_config.hpp"  // StreamLayout, place_tile
#include "core/frame_view.hpp"      // FrameView, TensorSet
#include "core/global_track.hpp"    // reid_cfg::kMaxSources
#include "core/net_perf.hpp"        // OverlayPerfWindow, EncPerfWindow
#include "core/track.hpp"           // TrajList
#include "gpu_box_drawer.hpp"
#include "net_encoder.h"
#include "net_preview.hpp"
#include "tracker.hpp"

struct StreamConfig;
struct LbGeom;
class Log;

class OverlaySink
{
public:
    // log: the process diagnostics, owned by main(); outlives the sink.
    explicit OverlaySink( const Log& log ) : log_( log ) {}

    // tensors: the pool's model-input tensors in slot order (the GPU
    // drawer imports each one once).
    void init( const StreamConfig& stream,
               const TensorSet& tensors,
               int model_w,
               int model_h,
               const LbGeom& primary_band,
               int nsrc = 1 );

    bool enabled() const { return preview_.has_value() || enc_.has_value(); }

    // Draws the overlay into the frame's tensor and publishes it; returns
    // the publish time, ms (0 when no viewer is connected).
    double draw_and_publish( const FrameView& frame,
                             const DetList& dets,
                             const TrajList& trajs,
                             const Tracker& tracker,
                             bool defer_encode = false ) noexcept;

    // NO SIGNAL idle publish (h264 path only): forwards to the encoder's
    // idle path; the raw NUT preview keeps its live-only diagnostic role.
    void publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept;
    void flush_dirty() noexcept;
    void reset_source( int src_idx ) noexcept;

    OverlayPerfWindow take_perf() noexcept;
    bool has_enc_perf() const noexcept { return enc_.has_value(); }
    EncPerfWindow take_enc_perf() noexcept;

private:
    void init_gpu_drawer( bool cpu_draw, const TensorSet& tensors );
    void reset_all_sources() noexcept;
    void sync_viewer_session() noexcept;
    float tile_scale( const FrameView& frame ) const noexcept;
    bool draw_overlay_into_tensor( const FrameView& frame,
                                   const DetList& dets,
                                   const TrajList& trajs,
                                   const Tracker& tracker ) noexcept;

private:
    const Log& log_;
    std::optional<NetPreview> preview_ {};
    std::optional<NetEncoder> enc_ {};
    std::optional<GpuBoxDrawer> drawer_ {};
    BoxMapper mapper_ {};
    GpuBoxList boxes_ {};
    CalloutList callouts_ {};
    std::array<CalloutHistory, reid_cfg::kMaxSources> callout_hist_ {};
    std::array<TrackBoxFilters, reid_cfg::kMaxSources> track_filt_ {};
    // Person boxes come from the Kalman posterior, already smooth in position:
    // a higher still cutoff and speed gain than the raw boxes of BoxSmoother keep
    // the box on a walking person (the dead zone still holds a standing one still).
    const BoxFilterParams kTrackFilter { 1.0f, 0.05f, 1.0f, 0.01f, 2.0f, 0.5f };
    StreamLayout layout_ {};  // the h264 stream geometry (tiled_): the callout tile scale
    bool tiled_ = false;
    LabelLang label_lang_ = LabelLang::En;
    int label_scale_ = 1;
    int model_w_ = 0;
    int model_h_ = 0;
    uint32_t stride_bytes_ = 0;
    OverlayPerfWindow perf_ {};
    uint64_t prev_fence_tmo_ = 0;
    bool was_viewer_live_ = false;
    uint32_t last_enc_epoch_ = 0;
    uint32_t last_prev_epoch_ = 0;
};
