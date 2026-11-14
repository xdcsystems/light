// FrameComposer: assembles the picture the encoder reads, with RGA, from
// dma-bufs only (no CPU pixel copies on the frame path).
//   single view: the letterbox band of the NPU tensor (RGB888) is
//     converted into the ring slot (NV12) the encoder reads next;
//   multi-view: each source's band goes into its cell of a persistent
//     composite canvas (2x1 / 2x2); an encoded frame copies the whole
//     canvas into the ring slot, a frame closer than the pacing step only
//     updates its cell (out == nullptr);
//   idle: the NO SIGNAL canvas (gray + IdlePlate, drawn once at init) is
//     copied into the slot (single view) or into the absent cells.
// Before a band lands in a slot / cell whose last band differs, the
// slot / cell is refilled gray: plate or old-band pixels must not survive
// around the new picture.
//
// Owns the composite canvas and the idle canvas (RAII dma-bufs: the ctor
// allocates or throws std::runtime_error). The geometry is StreamLayout's
// (place_tile / cell_rect, host-tested). Thread: the pipeline thread only.

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

    // Live frame of source src_idx: the band of tensor_fd into ring slot
    // `slot` (buffer `out`); multi-view: into the canvas cell, then the
    // canvas into `out` unless out == nullptr. false on an RGA failure.
    bool compose( int tensor_fd, const LbGeom& band, int src_idx, int slot, MppBuffer out );

    // NO SIGNAL frame: absent[s] (s < nabs; absent == nullptr or s >= nabs
    // counts as absent) selects the cells that get the plate. false on an
    // RGA failure.
    bool compose_idle( const bool* absent, int nabs, int slot, MppBuffer out );

private:
    const Log& log_;
    const StreamLayout lay_;
    const int model_w_;  // NPU tensor geometry (RGB888)
    const int model_h_;
    RkmppDrmPool<1> canvas_;  // persistent composite canvas (multi-view)
    RkmppDrmPool<1> idle_;    // NO SIGNAL canvas: gray + plate, stamped once at init
    std::array<LbGeom, kBands> bands_ {};
};
