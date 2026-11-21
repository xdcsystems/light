// Non-overlapping integer-aligned track label callout placement inside the
// FrameComposer even crop band (or output tile rect). Shared 1:1 by GpuBoxDrawer
// and OverlaySink CPU path.

#pragma once

#include <cstdint>

#include "core/geometry.hpp"    // LbGeom
#include "core/glyph_font.hpp"  // glyph_font::kLabelCap
#include "core/track.hpp"       // TrajList
#include "core/pod_list.hpp"    // PodList

// Half-open integer pixel rectangle [x0, x1) x [y0, y1).
struct CalloutRect
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// One track label: the box it belongs to, the label rect (layout only: nothing is
// drawn there but the text -- no plate), the L-connector and the text. The text
// is drawn at `scale` (atlas px -> target px) from the pen (text_x, text_y) =
// the line top-left: white glyphs over the dark outline baked into the atlas
// (glyph_font::kOutlinePx = 2 atlas px).
struct CalloutItem
{
    CalloutRect box {};
    CalloutRect label {};
    CalloutRect segs[2] {};
    int n_segs = 0;
    int text_x = 0;
    int text_y = 0;
    float scale = 0.5f;
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
        // Text size (font px) per --label-scale step on the output canvas: 11 px DejaVu
        // has ~8 px capitals, the size of the former 1-px-stroke bitmap font.
        kLabelPx = 11,
        // The gray of the dark outline around the white text (#91 item 5), shared by
        // the GPU and the CPU renderers. Its shape and alpha are baked into the A plane
        // of the atlas (glyph_font::kOutlinePx, kOutlineAlpha; #108).
        kOutlineGray = 16
    };

public:
    bool lang_ru = false;
    float scale = 0.5f;  // atlas px -> target px: callout_text_scale()
};

// The 0.5f default scales (CalloutItem, CalloutConfig, callout.cpp clean_scale) and
// the layout unit (2 * scale, 1 px at the default) assume the atlas is baked at
// 2x the default text size: rebaking at another ATLAS_PX must revisit them.
static_assert( glyph_font::kAtlasPx == 2 * CalloutConfig::kLabelPx, "callout defaults assume a 2x atlas" );

// The glyph scale that gives every source the same letter size on the output
// canvas (#91 item 5): CalloutConfig::kLabelPx px per --label-scale step, divided
// by tile_scale (1.0 when rendering 1:1 directly on the output canvas or raw NUT
// tensor). label_scale is clamped to 1..4; a non-positive or non-finite tile_scale
// counts as 1.
float callout_text_scale( int label_scale, float tile_scale = 1.0f ) noexcept;

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
// inside lb_even (even crop matching FrameComposer) minus 2 px margin.
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
