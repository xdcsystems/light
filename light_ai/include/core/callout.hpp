// Non-overlapping integer-aligned track label callout placement inside the
// FrameComposer even crop band. Shared 1:1 by GpuBoxDrawer and OverlaySink CPU path.

#pragma once

#include <cstdint>

#include "core/geometry.hpp"    // LbGeom
#include "core/glyph_font.hpp"  // GlyphFont::kLabelCap
#include "pod_list.hpp"    // PodList
#include "core/track.hpp"       // TrajList

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
    CalloutRect box {};
    CalloutRect pill {};
    CalloutRect segs[2] {};
    int n_segs = 0;
    int text_x = 0;
    int text_y = 0;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    char text[GlyphFont::kLabelCap] {};
    int text_bytes = 0;
};

using CalloutList = PodList<CalloutItem, 8>;

struct CalloutConfig
{
    bool lang_ru = false;
};

class CalloutLayout
{
public:
    // Computes non-overlapping callout pills and L-connectors for active tracks
    // inside lb_even (even crop matching FrameComposer::compose) minus 2 px margin.
    static void compute( const TrajList& trajs,
                         const LbGeom& lb,
                         float sx,
                         float sy,
                         const CalloutConfig& cfg,
                         CalloutList& out ) noexcept;
};
