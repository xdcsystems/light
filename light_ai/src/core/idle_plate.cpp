// The NO SIGNAL plate (core/idle_plate.hpp) on the label font's glyph walk
// (glyph_font::raster_text): the fill plane only, never the outline.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/glyph_font.hpp"
#include "core/idle_plate.hpp"

namespace
{
    constexpr char kText[] = "NO SIGNAL";
    constexpr int kTextBytes = static_cast<int>( sizeof( kText ) ) - 1;

    // The capital ink height of the atlas, atlas px: the padded rect of 'N' minus its border.
    constexpr int cap_ink() noexcept
    {
        for ( int i = 0; i < glyph_font::kGlyphCount; ++i )
        {
            if ( glyph_font::kCodepoints[i] == 0x4Eu )
                return glyph_font::kGlyphMetrics[i].h - 2 * glyph_font::kGlyphBorder;
        }
        return 0;
    }
    static_assert( cap_ink() > 0, "the charset has no 'N'" );

    // The ink box of the label with its pen at (0, 0): the union of the glyphs' ink
    // rects (the padded rects minus the border), float px.
    struct InkBox
    {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };

    InkBox ink_box( float sc ) noexcept
    {
        InkBox b { 1e9f, 1e9f, -1e9f, -1e9f };
        const float border = static_cast<float>( glyph_font::kGlyphBorder ) * sc;
        glyph_font::for_each_glyph( kText, kTextBytes, 0.0f, 0.0f, sc,
                                    [&b, sc, border]( int, const glyph_font::GlyphMetric& m, float gx, float gy ) {
                                        b.x0 = std::min( b.x0, gx + border );
                                        b.y0 = std::min( b.y0, gy + border );
                                        b.x1 = std::max( b.x1, gx + static_cast<float>( m.w ) * sc - border );
                                        b.y1 = std::max( b.y1, gy + static_cast<float>( m.h ) * sc - border );
                                    } );
        return b;
    }
}  // namespace

const char* IdlePlate::text() const noexcept
{
    return kText;
}

float IdlePlate::scale() const noexcept
{
    return static_cast<float>( kCapPx ) / static_cast<float>( cap_ink() );
}

IdlePlate::Pen IdlePlate::pen( const StreamLayout& layout ) const noexcept
{
    const InkBox b = ink_box( scale() );
    return { 0.5f * ( static_cast<float>( layout.tile_w ) - ( b.x0 + b.x1 ) ),
             0.5f * ( static_cast<float>( layout.tile_h ) - ( b.y0 + b.y1 ) ) };
}

void IdlePlate::draw( const Plane8& y_plane, const StreamLayout& layout ) const noexcept
{
    if ( y_plane.p == nullptr )
        return;
    const float sc = scale();
    const Pen pen0 = pen( layout );
    for ( int t = 0; t < layout.nsrc; ++t )
    {
        const int ox = ( t % 2 ) * layout.tile_w;
        const int oy = ( t / 2 ) * layout.tile_h;
        // The tile, clipped to the plane.
        const int w = std::min( layout.tile_w, y_plane.stride - ox );
        const int h = std::min( layout.tile_h, y_plane.h - oy );
        if ( w <= 0 || h <= 0 )
            continue;
        uint8_t* origin = y_plane.p + static_cast<size_t>( oy ) * static_cast<size_t>( y_plane.stride ) + ox;
        const int stride = y_plane.stride;
        glyph_font::raster_text( kText, kTextBytes, pen0.x, pen0.y, sc, w, h,
                                 [origin, stride]( int x, int y, const glyph_font::Texel2& s ) {
                                     if ( s.fill <= 0.0f )
                                         return;  // the fill plane only: no outline, no halo
                                     uint8_t& v = origin[static_cast<size_t>( y ) * static_cast<size_t>( stride ) + x];
                                     const float cur = static_cast<float>( v );
                                     const float out = cur + ( static_cast<float>( kLuma ) - cur ) * s.fill / 255.0f;
                                     v = static_cast<uint8_t>( std::lround( std::clamp( out, 0.0f, 255.0f ) ) );
                                 } );
    }
}
