// Fixed 9x12 8-bit anti-aliased bitmap font (ASCII + Cyrillic) with bounds-checked UTF-8
// decoding and 1-px-padded atlas builder for zero-heap GL_LINEAR GPU and CPU label rendering.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace glyph_font
{
    enum
    {
        kGlyphW = 9,
        kGlyphH = 12,
        kGlyphBytes = kGlyphW * kGlyphH,
        kCellPad = 1,
        kCellW = kGlyphW + 2 * kCellPad,
        kCellH = kGlyphH + 2 * kCellPad,
        kSlotCount = 162,
        kAtlasCols = 16,
        kAtlasRows = ( kSlotCount + kAtlasCols - 1 ) / kAtlasCols,
        kAtlasW = kAtlasCols * kCellW,
        kAtlasH = kAtlasRows * kCellH,
        kAtlasBytes = kAtlasW * kAtlasH,
        kLabelCap = 32
    };

    // 9x12 8-bit alpha coverage bitmap for slot [0..kSlotCount-1]. Slot 0 is solid 0xFF.
    const uint8_t* glyph_alpha( int idx ) noexcept;

    // Maps Unicode codepoint to slot [1..161]; unknown -> '?' slot.
    int glyph_index( uint32_t cp ) noexcept;

    // Decodes next UTF-8 codepoint starting at pos; advances pos cleanly on any invalid
    // or truncated byte sequence and returns '?'. Returns 0 when pos reaches len.
    uint32_t next_utf8( const char* str, int len, int& pos ) noexcept;

    // Counts decoded glyphs in a UTF-8 string.
    int text_len( std::string_view text ) noexcept;

    // Populates kAtlasW x kAtlasH GL_ALPHA atlas with 1-px padding per glyph cell.
    void build_atlas( std::array<uint8_t, kAtlasBytes>& atlas ) noexcept;

    // Texture atlas UVs for a glyph cell interior, or the center of padded solid slot 0.
    void glyph_uv( int idx, float& u0, float& v0, float& u1, float& v1 ) noexcept;
    void solid_uv( float& u, float& v ) noexcept;

    // Zero-heap person label formatting into dst[dst_cap]; returns byte length.
    int format_person( char* dst, int dst_cap, int id, bool lang_ru ) noexcept;

}  // namespace glyph_font
