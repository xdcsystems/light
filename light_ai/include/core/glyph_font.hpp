// Fixed 8x8 1-bpp bitmap font (ASCII + Cyrillic) with bounds-checked UTF-8 decoding
// and atlas builder for zero-heap GPU/CPU callout label rendering.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace glyph_font
{
    enum
    {
        kGlyphW = 8,
        kGlyphH = 8,
        kSlotCount = 162,
        kAtlasW = 128,
        kAtlasH = 96,
        kAtlasCols = kAtlasW / kGlyphW,
        kAtlasBytes = kAtlasW * kAtlasH,
        kLabelCap = 32
    };

    // 8-byte LSB-left row bitmap for slot [0..kSlotCount-1]. Slot 0 is solid 0xFF.
    const uint8_t* glyph_rows( int idx ) noexcept;

    // Maps Unicode codepoint to slot [1..161]; unknown -> '?' slot.
    int glyph_index( uint32_t cp ) noexcept;

    // Decodes next UTF-8 codepoint starting at pos; advances pos cleanly on any invalid
    // or truncated byte sequence and returns '?'. Returns 0 when pos reaches len.
    uint32_t next_utf8( const char* str, int len, int& pos ) noexcept;

    // Counts decoded glyphs in a UTF-8 string.
    int text_len( std::string_view text ) noexcept;

    // Populates 128x96 GL_ALPHA atlas (0x00 transparent, 0xFF opaque).
    void build_atlas( std::array<uint8_t, kAtlasBytes>& atlas ) noexcept;

    // Texture atlas UVs for a glyph cell, or the center of solid slot 0.
    void glyph_uv( int idx, float& u0, float& v0, float& u1, float& v1 ) noexcept;
    void solid_uv( float& u, float& v ) noexcept;

    // Zero-heap person label formatting into dst[dst_cap]; returns byte length.
    int format_person( char* dst, int dst_cap, int id, bool lang_ru ) noexcept;

}  // namespace glyph_font
