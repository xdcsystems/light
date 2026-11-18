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

#include "box_mapper.h"          // BoxMapper
#include "config/config.hpp"     // LabelLang
#include "core/box_envelope.hpp" // BoxEnvelope
#include "core/callout.hpp"      // CalloutHistory, CalloutList
#include "core/detection.hpp"    // DetList
#include "core/frame_view.hpp"   // FrameView, TensorSet
#include "core/global_track.hpp" // reid_cfg::kMaxSources
#include "core/track.hpp"        // TrajList
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
                             const Tracker& tracker ) noexcept;

    // NO SIGNAL idle publish (h264 path only): forwards to the encoder's
    // idle path; the raw NUT preview keeps its live-only diagnostic role.
    void publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept;

private:
    struct TrackDispStab
    {
        int id = 0;
        uint32_t gen = 0;
        BoxEnvelope env {};
    };
    using TrackStabList = PodList<TrackDispStab, TrajList::cap>;

    void init_gpu_drawer( bool cpu_draw, const TensorSet& tensors );
    int find_stab_idx( const TrackStabList& list, int id, uint32_t gen ) const noexcept;
    void stabilize_display_tracks( int src_idx, TrajList& disp ) noexcept;
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
    std::array<TrackStabList, reid_cfg::kMaxSources> track_stab_ {};
    LabelLang label_lang_ = LabelLang::En;
    int model_w_ = 0;
    int model_h_ = 0;
    uint32_t stride_bytes_ = 0;
};
