// Proportional DejaVu Sans bitmap font (ASCII + Cyrillic + punctuation), 8-bit
// alpha atlas with per-glyph metrics. Drop-in replacement for the old fixed
// 7x12 font: API names are preserved, and the compatibility constants below
// map the old cell math onto the single-column atlas (one 18x18 cell per row).

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "core/glyph_font_table.hpp"
namespace glyph_font
{
    enum : int
    {
        // Cell geometry for the single-column atlas (compat with old call sites).
        kGlyphW = kGlyphWMax,
        kGlyphBytes = kGlyphWMax * kGlyphH,
        kCellPad = 0,
        kCellW = kGlyphWMax,
        kCellH = kGlyphH,
        kAtlasCols = 1,
        kAtlasRows = kGlyphCount,
        kSlotCount = kGlyphCount,
        kLabelCap = 32,

        // Virtual blank glyph: space has advance but no ink, not stored in the atlas.
        kGlyphSpace = kGlyphCount,
        kSpaceAdv = 5,

        // Guaranteed-opaque texel for solid frame quads (inside glyph 'f').
        kSolidX = 2,
        kSolidY = 744
    };

    // 18x18 alpha cell for glyph idx; bounds-checked, falls back to '?'.
    const uint8_t* glyph_alpha( int idx ) noexcept;

    // Unicode codepoint -> glyph index; space -> kGlyphSpace; unknown -> '?'.
    int glyph_index( uint32_t cp ) noexcept;

    // Decodes the next UTF-8 codepoint at pos; advances pos cleanly on any
    // invalid or truncated sequence (returns '?'); returns 0 at end of input.
    uint32_t next_utf8( const char* str, int len, int& pos ) noexcept;

    // Counts decoded glyphs in a UTF-8 string.
    int text_len( std::string_view text ) noexcept;

    // Copies the built-in atlas into a GL_ALPHA texture buffer (compat wrapper;
    // the GPU path switches to kAtlas directly in the following parts).
    void build_atlas( std::array<uint8_t, kAtlasBytes>& atlas ) noexcept;

    // Ink-rect UVs of glyph idx: u0/v0 = ink origin, u1/v1 = ink end (texel units).
    void glyph_uv( int idx, float& u0, float& v0, float& u1, float& v1 ) noexcept;

    // Center UV of the guaranteed-opaque texel (kSolidX, kSolidY).
    void solid_uv( float& u, float& v ) noexcept;

    // Zero-heap person label formatting into dst[dst_cap]; returns byte length.
    int format_person( char* dst, int dst_cap, int id, bool lang_ru ) noexcept;

    // Width of a UTF-8 string in atlas pixels: sum of per-glyph advances.
    int text_width( std::string_view text ) noexcept;

    // Per-glyph metrics (dx, dy, w, h, adv); bounds-checked, space allowed.
    const GlyphMetric& glyph_metrics( int idx ) noexcept;

}  // namespace glyph_font
