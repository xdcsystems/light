// BitmapFont: short labels rasterized into an 8-bit plane (the Y plane of
// an NV12 buffer) with the stb_easy_font stroke glyphs. The library emits
// 1-px-thick axis-aligned stroke quads, so rasterization is a scaled bbox
// fill: smooth at any integer scale.
//
// stb_easy_font is a header-only external with file statics of its own: it
// is included only from src/core/bitmap_font.cpp, never from this header
// (README, rules: the externals exception).
//
// Text is copied into a 32-byte buffer (the library API is char*-based);
// longer strings are cut. Characters outside the font render as blanks.
// The caller owns any DMA cache sync of the plane.

#pragma once

#include <cstdint>

#include "core/idle_plate.hpp"  // Plane8

class BitmapFont
{
    enum
    {
        // Aligned with kTextCap=32: worst-case glyphs produce <= 12 quads/char,
        // 31 chars * 12 quads * 4 verts * 16 bytes = 23808 <= 24576 (384 quads).
        kVertexBytes = 24576
    };

public:
    // Rendered text size in plane pixels at `scale`; 0 for nullptr.
    int width( const char* s, int scale ) const;
    int height( const char* s, int scale ) const;

    // Renders `s` with its glyph-area top left at (x, y), every stroke quad
    // scaled by `scale`, pixels set to `val`. Clipped to the plane: nothing
    // is written outside [0, stride) x [0, h).
    void stamp( const Plane8& plane, int x, int y, const char* s, int scale, uint8_t val );

private:
    // stb vertex output. A member, not a local of stamp(): the 8 KiB live
    // wherever the font lives (IdlePlate is a local of the one-time plate
    // draw at init, so there it is stack, once).
    alignas( 16 ) char vbuf_[kVertexBytes] {};
};
