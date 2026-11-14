// BitmapFont: short labels rasterized into an 8-bit plane (the Y plane of
// an NV12 buffer) with the stb_easy_font stroke glyphs. The library emits
// 1-px-thick axis-aligned stroke quads, so rasterization is a scaled bbox
// fill: smooth at any integer scale.
//
// stb_easy_font is a header-only external with file statics of its own: it
// is included only from src/core/bitmap_font.cpp, never from this header
// (README, rules: the externals exception). SDK-free, host-tested
// (tests/test_idle_plate.cpp).
//
// Text is copied into a 64-byte buffer (the library API is char*-based);
// longer strings are cut. Characters outside the font render as blanks.
// The caller owns any DMA cache sync of the plane.

#pragma once

#include <cstdint>

// An 8-bit plane: row stride (also the clip width) and height in rows.
struct Plane8
{
    uint8_t* p = nullptr;
    int stride = 0;
    int h = 0;
};

class BitmapFont
{
    enum
    {
        kVertexBytes = 8192  // 128 quads of 4 vertices: enough for any short label
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
