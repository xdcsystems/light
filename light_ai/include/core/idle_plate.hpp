// IdlePlate: the NO SIGNAL picture of the idle canvas -- "NO SIGNAL" at 3x
// scale, luma 16, centered in every tile of the stream layout (single
// view: once). Drawn once at init into the Y plane of the gray-filled idle
// canvas; publish_idle() RGA-copies tiles of it into the stream.
//
// A one-time CPU raster at init: the README zero-copy rule governs the
// FRAME path, and this follows the gray-letterbox memset precedent
// (init-time only, never per frame). SDK-free, host-tested
// (tests/test_idle_plate.cpp); the DMA cache sync is the caller's.

#pragma once

#include <cstdint>

#include "core/bitmap_font.hpp"
#include "core/encoder_config.hpp"

class IdlePlate
{
    enum
    {
        kScale = 3,
        kLuma = 16
    };

public:
    // y_plane: the canvas Y plane, stride = layout.w, h = layout.vstride.
    void draw( const Plane8& y_plane, const StreamLayout& layout );

    // Top left of the label inside one tile (the same offset in every tile).
    int text_x( const StreamLayout& layout ) const;
    int text_y( const StreamLayout& layout ) const;
    int text_w() const;  // label size in pixels
    int text_h() const;

    uint8_t luma() const { return kLuma; }

private:
    BitmapFont font_;
};
