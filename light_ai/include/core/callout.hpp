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

struct CalloutItem
{
    enum
    {
        kGlyphScale = 1,
        kGlyphW = glyph_font::kGlyphW * kGlyphScale,
        kGlyphH = glyph_font::kGlyphH * kGlyphScale,
    };

    CalloutRect box {};
    CalloutRect pill {};
    CalloutRect segs[2] {};
    int n_segs = 0;
    int text_x = 0;
    int text_y = 0;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    char text[glyph_font::kLabelCap] {};
    int text_bytes = 0;
};

using CalloutList = PodList<CalloutItem, 8>;

struct CalloutConfig
{
    bool lang_ru = false;
};

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

// Computes non-overlapping callout pills and L-connectors for active tracks
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
