// FrameComposer: assembles the picture the encoder reads, with RGA, from
// dma-bufs only (no CPU pixel copies on the frame path).
//   single view: the letterbox band of the NPU tensor (RGB888) is
//     converted into the ring slot (NV12) the encoder reads next;
//   multi-view: each source's band goes into its cell of a persistent
//     composite canvas (2x1 / 2x2); an encoded frame copies the whole
//     canvas into the ring slot, a frame closer than the pacing step only
//     updates its cell (out == nullptr);
//   idle: the NO SIGNAL canvas (gray + IdlePlate, drawn once at init) is
//     copied into the slot (single view) or into the absent cells that do
//     not hold the plate already (multi-view; out == nullptr: canvas only);
//   session start (multi-view): reset_canvas() fills the whole canvas gray,
//     so no picture of a previous session reaches the new one (#24).
// Before a band lands in a slot / cell whose last band differs, the
// slot / cell is refilled gray: plate or old-band pixels must not survive
// around the new picture.
//
// Owns the composite canvas and the idle canvas (RAII dma-bufs: the ctor
// logs the stream layout, then allocates or throws std::runtime_error).
// The geometry is StreamLayout's (place_tile / cell_rect, host-tested).
// Thread: the pipeline thread only.

#pragma once

#include <array>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers.
#include <rockchip/rk_mpi.h>

#include "core/encoder_config.hpp"
#include "core/geometry.hpp"
#include "rkmpp_drm_pool.hpp"

class Log;

class FrameComposer
{
public:
    enum
    {
        kBands = 4  // last band per ring slot (single view) / per grid cell (multi-view)
    };

public:
    // log: the process diagnostics, owned by main(); outlives the composer.
    FrameComposer( const EncoderConfig& cfg, const Log& log );
    FrameComposer( const FrameComposer& ) = delete;
    FrameComposer& operator=( const FrameComposer& ) = delete;

    // Live frame: the band of tensor_fd, placed by tp (place_tile: the
    // caller needs tp.cell too), into ring slot `slot` (buffer `out`);
    // multi-view: into the canvas cell tp.cell, then the canvas into `out`
    // unless out == nullptr. false on an RGA failure.
    bool compose( int tensor_fd, const TilePlacement& tp, int slot, MppBuffer out );

    // NO SIGNAL frame: tile_absent( absent, nabs, s ) selects the cells that
    // get the plate. Multi-view: out == nullptr updates the canvas only.
    // Single view: out is required (the idle canvas is the frame). false on
    // an RGA failure.
    bool compose_idle( const bool* absent, int nabs, int slot, MppBuffer out );

    // Session start (multi-view; single view: no-op): the whole composite
    // canvas gray, no cell remembers a band or the plate.
    void reset_canvas();

private:
    const Log& log_;
    const StreamLayout lay_;
    const int model_w_;  // NPU tensor geometry (RGB888)
    const int model_h_;
    RkmppDrmPool<1> canvas_;  // persistent composite canvas (multi-view)
    RkmppDrmPool<1> idle_;    // NO SIGNAL canvas: gray + plate, stamped once at init
    std::array<LbGeom, kBands> bands_ {};
};
