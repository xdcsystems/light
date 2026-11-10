// Overlay drawing (Mali G610 EGL/GBM or CPU fallback) and network
// preview publishing (NetEncoder H.264 / NetPreview raw NUT).
// Extracted from Pipeline to keep the composition root focused on
// source capture, NPU scheduling and per-source state.

#pragma once

#include <optional>
#include <vector>

#include "gpu_box_drawer.hpp"
#include "net_encoder.h"
#include "net_preview.hpp"
#include "tracker.hpp"
#include "yolo_postprocess.hpp"

struct Config;
struct LbGeom;
struct Slot;
class RknnEngine;

class OverlaySink
{
public:
    void init( const Config& cfg,
               const std::vector<RknnEngine>& npus,
               int model_w,
               int model_h,
               const LbGeom& primary_band,
               int nsrc = 1 );

    bool enabled() const { return preview_.has_value() || enc_.has_value(); }

    double draw_and_publish( Slot& sl,
                             int ps,
                             const DetList& dets,
                             const TrajList& trajs,
                             const Tracker& tracker );

private:
    void init_gpu_drawer( const Config& cfg, const std::vector<RknnEngine>& npus );

private:
    std::optional<NetPreview> preview_ {};
    std::optional<NetEncoder> enc_ {};
    std::optional<GpuBoxDrawer> drawer_ {};
    GpuBoxList boxes_ {};
    int model_w_ = 0;
    int model_h_ = 0;
};
