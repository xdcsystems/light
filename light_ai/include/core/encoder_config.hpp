// EncoderConfig + StreamLayout: everything NetEncoder needs to know before
// it opens the codec, as one named object instead of an 8-parameter ctor
// (five ints in a row: fps and port could be swapped without a compiler error).
//
// StreamLayout is the stream geometry, pure integer math (host-tested):
//   single view (nsrc 1): the stream is the visible letterbox band of the
//     tensor at its native size, the width 16-aligned;
//   multi-view (nsrc 2..4): one tile_w x tile_h cell per source, 4:3 of the
//     model input, in a Side-by-Side (2x1) or Grid (2x2) canvas.
// SDK-free: the RGA calls take these numbers, nothing here knows RGA.

#pragma once

#include <cstdint>
#include <string>

#include "core/geometry.hpp"

struct StreamLayout
{
    enum
    {
        kMaxTiles = 4  // 2x2 grid max: the canvas and the per-tile bands are sized for 4
    };

public:
    int w = 0;  // stream (encoder) size
    int h = 0;
    int tile_w = 0;  // one cell; single view: the band, the width 16-aligned
    int tile_h = 0;
    int nsrc = 1;     // tiles in the canvas, 1..kMaxTiles
    int vstride = 0;  // NV12 vertical stride: h 16-aligned (MPP encoders want it)
    LbGeom band {};   // visible letterbox band of the tensor, clamped into it
};

// nsrc <= 1: single view; nsrc > kMaxTiles is clamped to kMaxTiles
// (the caller logs the clamp).
StreamLayout make_stream_layout( int model_w, int model_h, const LbGeom& band, int nsrc );

// Cell s of the 2x1 / 2x2 grid, in stream pixels.
LbGeom cell_rect( const StreamLayout& layout, int s );

// Where one source frame goes: the tensor crop and the canvas rectangle,
// both even-aligned for NV12 (the former prep_frame math).
struct TilePlacement
{
    int cell = 0;  // grid cell (0 in single view; an out-of-range src_idx falls back to 0)
    LbGeom src {};  // crop of the tensor
    LbGeom dst {};  // rectangle in the stream frame / composite canvas
};

// band: the frame's letterbox band in the tensor; w or h == 0 -> layout.band.
// The band is scaled into its cell with band_geom() unless it already fits exactly.
TilePlacement place_tile( const StreamLayout& layout, const LbGeom& band, int src_idx );

struct EncoderConfig
{
    bool vfr() const { return fps <= 0; }  // --net-fps 0: follow the source, pts from dequeue times
    // Rate-control fps: VFR still needs a number (0/1 reaches MPP as the auto marker and the
    // CBR per-frame budget collapses): nominal 25, 25 per source in multi-view.
    int nominal_fps() const { return vfr() ? ( layout.nsrc > 1 ? 25 * layout.nsrc : 25 ) : fps; }
    int gop() const { return vfr() ? nominal_fps() : fps; }  // 1 s: a late joiner waits at most one GOP

public:
    StreamLayout layout {};
    int model_w = 0;  // NPU tensor geometry (RGB888)
    int model_h = 0;
    int fps = 25;            // stream fps; <= 0 = VFR
    int port = 1234;         // TCP pull listen port / UDP destination port
    std::string host;        // UDP push destination; empty = TCP pull
    int bitrate_kbps = 12000;
};
