// Overlay drawing (Mali G610 EGL/GBM or CPU fallback) and network
// preview publishing (NetEncoder H.264 / NetPreview raw NUT).
// Extracted from Pipeline to keep the composition root focused on
// source capture, NPU scheduling and per-source state.
// On the H.264 path, boxes, trails and labels are rendered 1:1 directly onto
// NetEncoder's RGB888 output canvas (StreamLayout pixels, zero CPU copies on
// the GPU path); on the raw NUT preview path, they render 1:1 into the NPU
// input tensor.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

#include "config/config.hpp"        // LabelLang
#include "core/box_filter.hpp"      // TrackBoxFilters
#include "core/callout.hpp"         // CalloutHistory, CalloutList
#include "core/detection.hpp"       // DetList
#include "core/encoder_config.hpp"  // StreamLayout, make_overlay_target
#include "core/frame_view.hpp"      // FrameView, TensorSet
#include "core/global_track.hpp"    // reid_cfg::kMaxSources
#include "core/net_perf.hpp"        // OverlayPerfWindow, EncPerfWindow
#include "core/track.hpp"           // TrajList
#include "core/tracker.hpp"
#include "net/net_encoder.hpp"
#include "net/net_preview.hpp"
#include "render/box_mapper.hpp"    // BoxMapper
#include "render/gpu_box_drawer.hpp"

struct StreamConfig;
struct LbGeom;
class Log;

class OverlaySink
{
public:
    // log: the process diagnostics, owned by main(); outlives the sink.
    explicit OverlaySink( const Log& log ) : log_( log ) {}

    // tensors: the pool's model-input tensors in slot order (imported by the
    // GPU drawer only on the raw NUT preview path; H.264 imports the output canvas).
    void init( const StreamConfig& stream,
               const TensorSet& tensors,
               int model_w,
               int model_h,
               const LbGeom& primary_band,
               int nsrc = 1 );

    bool enabled() const { return preview_.has_value() || enc_.has_value(); }
    bool viewer_live() const noexcept;
    void release_worker_context() noexcept;

    // Split path for async StreamOutput: prepare_overlay() runs on the pipeline
    // thread (reads Tracker/filters), render_and_publish() runs on the output thread.
    void prepare_overlay( const FrameView& frame,
                          const DetList& dets,
                          const TrajList& trajs,
                          const Tracker& tracker,
                          GpuBoxList& boxes_out,
                          CalloutList& callouts_out ) noexcept;
    double render_and_publish( const FrameView& frame,
                               const GpuBoxList& boxes,
                               const CalloutList& callouts,
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
    void init_gpu_drawer_canvas( bool cpu_draw, const CanvasBuffer& canvas );
    void init_gpu_drawer_tensors( bool cpu_draw, const TensorSet& tensors );
    void reset_all_sources() noexcept;
    void sync_viewer_session() noexcept;
    void draw_gpu_slot( int slot, const GpuBoxList& boxes, const CalloutList& callouts ) noexcept;
    void draw_prepared_into_canvas( const LbGeom& dst_band,
                                    const GpuBoxList& boxes,
                                    const CalloutList& callouts ) noexcept;
    bool draw_prepared_into_tensor( const FrameView& frame,
                                    const GpuBoxList& boxes,
                                    const CalloutList& callouts ) noexcept;

private:
    const Log& log_;
    std::optional<NetPreview> preview_ {};
    // Declaration order matters: drawer_ imports enc_'s RGB888 canvas dma-buf,
    // so drawer_ must be declared AFTER enc_ to release its EGLImage/FBO first.
    std::optional<NetEncoder> enc_ {};
    std::optional<GpuBoxDrawer> drawer_ {};
    BoxMapper mapper_ {};
    std::array<CalloutHistory, reid_cfg::kMaxSources> callout_hist_ {};
    std::array<TrackBoxFilters, reid_cfg::kMaxSources> track_filt_ {};
    StreamLayout layout_ {};  // h264 stream geometry (tiled_): 1:1 overlay target
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
    std::atomic<uint32_t> session_reset_gen_ { 0 };
    uint32_t applied_reset_gen_ = 0;
};
