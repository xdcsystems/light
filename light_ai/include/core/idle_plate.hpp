// IdlePlate: the NO SIGNAL picture of the idle canvas -- "NO SIGNAL" in the
// callout label font (glyph_font, DejaVu Sans; #108) with 21 px capitals, luma 16,
// no outline (dark text on the gray canvas), its ink box centered in every tile
// of the stream layout (single view: once). Drawn once at init into the Y plane
// of the gray-filled idle canvas; publish_idle() RGA-copies tiles of it into the
// stream.
//
// A one-time CPU raster at init: the README zero-copy rule governs the
// FRAME path, and this follows the gray-letterbox memset precedent
// (init-time only, never per frame). SDK-free, host-tested
// (tests/test_idle_plate.cpp); the DMA cache sync is the caller's.

#pragma once

#include <cstdint>

#include "core/encoder_config.hpp"

// An 8-bit plane: row stride (also the clip width) and height in rows.
struct Plane8
{
    uint8_t* p = nullptr;
    int stride = 0;
    int h = 0;
};

class IdlePlate
{
    enum
    {
        kCapPx = 21,  // capital height on the canvas, px: the ink height of the label before #108 (3x stroke font)
        kLuma = 16
    };

public:
    // y_plane: the canvas Y plane, stride = layout.w, h = layout.vstride. Every
    // pixel under the text moves toward kLuma by the glyph fill coverage
    // (y += (kLuma - y) * fill / 255); nothing else is written, nothing outside
    // the plane or a tile.
    void draw( const Plane8& y_plane, const StreamLayout& layout ) const noexcept;

    // The label text and its glyph scale (atlas px -> canvas px): kCapPx over the
    // capital height of the atlas, from the glyph metrics.
    const char* text() const noexcept;
    float scale() const noexcept;

    // The pen (the line top-left) inside one tile, float px: the same in every
    // tile, so that the label's ink box is centered in the tile.
    float pen_x( const StreamLayout& layout ) const noexcept;
    float pen_y( const StreamLayout& layout ) const noexcept;

    uint8_t luma() const noexcept { return kLuma; }
};
