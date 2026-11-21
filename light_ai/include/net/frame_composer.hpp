// FrameComposer: assembles the picture the encoder reads, with RGA, from
// dma-bufs only (no CPU pixel copies on the frame path).
//   canvas_ (RGB888, lay_.w x lay_.h, stride lay_.w * 3): persistent RGB888
//     output canvas for both single-view and multi-view. Each source's band
//     is RGA-scaled into its tile (compose_tile), then OverlaySink draws
//     boxes, trails and labels 1:1 in stream pixels directly onto canvas_
//     (Mali G610 EGL/GBM or CPU fallback), and finally flush_canvas() does a
//     single 1:1 RGA RGB888->NV12 convert into the encoder ring slot `out`.
//   idle_ (NV12, lay_.w x lay_.vstride * 3 / 2): NO SIGNAL canvas (gray +
//     IdlePlate, drawn once at init). Single-view copies idle_ directly into
//     `out`; multi-view RGA-converts absent tiles from idle_ into canvas_ and
//     flushes canvas_ to `out`.
//   session start (multi-view): reset_canvas() fills the whole RGB888 canvas
//     gray, so no picture of a previous session reaches the new one.
// Before a band lands in a cell whose last band differs, the cell is refilled
// gray: plate or old-band pixels must not survive around the new picture.
//
// Owns the composite canvas and the idle canvas (RAII dma-bufs: the ctor
// logs the stream layout, then allocates or throws std::runtime_error).
// The geometry is StreamLayout's (place_tile / cell_rect, host-tested).
// Thread: the output worker thread only.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers.
#include <rockchip/rk_mpi.h>

#include "core/encoder_config.hpp"
#include "core/geometry.hpp"
#include "core/status.hpp"
#include "media/rkmpp_drm_pool.hpp"

class Log;

// Non-owning view of FrameComposer's persistent RGB888 output canvas dma-buf.
// Shared with OverlaySink (Mali G610 EGLImage import or CPU fallback draw).
struct CanvasBuffer
{
    int fd = -1;
    void* data = nullptr;
    size_t size = 0;
    int w = 0;
    int h = 0;
    uint32_t stride_bytes = 0;
};

class FrameComposer
{
public:
    enum
    {
        kBands = 8  // last band per grid cell (single view uses cell 0)
    };

public:
    // log: the process diagnostics, owned by main(); outlives the composer.
    FrameComposer( const EncoderConfig& cfg, const Log& log );
    FrameComposer( const FrameComposer& ) = delete;
    FrameComposer& operator=( const FrameComposer& ) = delete;

    // Non-owning view of the persistent RGB888 output canvas (valid for the
    // lifetime of FrameComposer; destroyed after OverlaySink's GpuBoxDrawer).
    CanvasBuffer canvas_buffer() const noexcept;

    // Live frame step 1: RGA-scales the band of tensor_fd (RGB888) into the
    // RGB888 output canvas at tp.dst (refilling the cell gray on band change).
    // Status::Failed on an RGA failure (imStrError is logged before return).
    Status compose_tile( int tensor_fd, const TilePlacement& tp );

    // Live / deferred step 2: 1:1 RGA RGB888 -> NV12 convert of the output
    // canvas into encoder ring buffer `out` (single-view and multi-view).
    Status flush_canvas( MppBuffer out );

    // NO SIGNAL frame: tile_absent( absent, nabs, s ) selects the cells that
    // get the plate. Multi-view: out == nullptr updates the canvas only.
    // Single view: out is required (the idle canvas is copied directly).
    // Status::Failed on an RGA failure (imStrError is logged before return).
    Status compose_idle( const bool* absent, int nabs, int slot, MppBuffer out );

    // Session start (multi-view; single view: no-op): the whole composite
    // canvas gray, no cell remembers a band or the plate.
    void reset_canvas();

private:
    const Log& log_;
    const StreamLayout lay_;
    const int model_w_;  // NPU tensor geometry (RGB888)
    const int model_h_;
    const int model_stride_px_;  // tensor row stride (EncoderConfig::model_stride_px, resolved)
    RkmppDrmPool<1> canvas_;     // persistent RGB888 output canvas (single-view + multi-view)
    RkmppDrmPool<1> idle_;       // NO SIGNAL NV12 canvas: gray + plate, stamped once at init
    std::array<LbGeom, kBands> bands_ {};
};
