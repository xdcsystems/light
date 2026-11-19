// Non-overlapping integer-aligned track label callout placement inside the
// FrameComposer even crop band. Shared 1:1 by GpuBoxDrawer and OverlaySink CPU path.

#pragma once

#include <cstdint>

#include "core/geometry.hpp"    // LbGeom
#include "core/glyph_font.hpp"  // glyph_font::kLabelCap
#include "core/track.hpp"       // TrajList
#include "pod_list.hpp"         // PodList

// Half-open integer tensor-pixel rectangle [x0, x1) x [y0, y1).
struct CalloutRect
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// One track label: the box it belongs to, the label rect (layout only: nothing is
// drawn there but the text -- no plate), the L-connector and the text. The text
// is drawn at `scale` (atlas px -> tensor px) from the pen (text_x, text_y) =
// the line top-left: white glyphs over an 8-way dark outline `outline` px wide.
struct CalloutItem
{
    CalloutRect box {};
    CalloutRect label {};
    CalloutRect segs[2] {};
    int n_segs = 0;
    int text_x = 0;
    int text_y = 0;
    float scale = 0.5f;
    float outline = 1.0f;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    char text[glyph_font::kLabelCap] {};
    int text_bytes = 0;
};

using CalloutList = PodList<CalloutItem, TrajList::cap>;  // one per track of a frame

struct CalloutConfig
{
    enum
    {
        kLabelPx = 16  // letter size per --label-scale step on the output tile
    };

public:
    bool lang_ru = false;
    float scale = 0.5f;  // atlas px -> tensor px: callout_text_scale()
};

// The glyph scale that gives every source the same letter size on the output
// tile (#91 item 5): CalloutConfig::kLabelPx px per --label-scale step, measured in the stream,
// divided by tile_scale (output tile px per tensor px: place_tile dst.h / src.h,
// 1 when there is no tiled stream). label_scale is clamped to 1..4; a
// non-positive or non-finite tile_scale counts as 1.
//   USB 4:3 band 640x480 -> 640x480 tile: 1.0  -> 0.5 (16 px letters in the tensor)
//   phone 360x640 band   -> 270x480 tile: 0.75 -> 0.667 (21.3 px in the tensor, 16 px on the tile)
float callout_text_scale( int label_scale, float tile_scale ) noexcept;

struct CalloutPrevSlot
{
    int id = 0;        // local TrackState::id
    uint32_t gen = 0;  // monotonic TrackState::gen
    int slot = -1;     // last winning proposal index [0..13]
};

struct CalloutHistory
{
    PodList<CalloutPrevSlot, TrajList::cap> entries {};
};

// Computes non-overlapping callout labels and L-connectors for active tracks
// inside lb_even (even crop matching FrameComposer::compose) minus 2 px margin.
// When hist is provided, keeps each track's prior slot across frames unless an overlap
// or edge clamp forces a move (prevents left/right flip-flop on 1-px box jitter).
void compute_callouts( const TrajList& trajs,
                       const LbGeom& lb,
                       float sx,
                       float sy,
                       const CalloutConfig& cfg,
                       CalloutHistory& hist,
                       CalloutList& out ) noexcept;

void compute_callouts( const TrajList& trajs,
                       const LbGeom& lb,
                       float sx,
                       float sy,
                       const CalloutConfig& cfg,
                       CalloutList& out ) noexcept;
