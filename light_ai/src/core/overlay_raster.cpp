// The CPU overlay raster (core/overlay_raster.hpp): boxes and callouts into an
// RGB888 image, the CPU twin of GpuBoxDrawer (gpu_box_drawer.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/callout.hpp"
#include "core/glyph_font.hpp"
#include "core/gpu_box.hpp"
#include "core/overlay_raster.hpp"

namespace
{
    void blend_px( uint8_t* px, uint8_t r, uint8_t g, uint8_t b, unsigned a ) noexcept
    {
        if ( a >= 255u )
        {
            px[0] = r;
            px[1] = g;
            px[2] = b;
            return;
        }
        px[0] = static_cast<uint8_t>( ( r * a + px[0] * ( 255u - a ) + 127u ) / 255u );
        px[1] = static_cast<uint8_t>( ( g * a + px[1] * ( 255u - a ) + 127u ) / 255u );
        px[2] = static_cast<uint8_t>( ( b * a + px[2] * ( 255u - a ) + 127u ) / 255u );
    }

    void fill_rect_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutRect& rect,
                        uint8_t r, uint8_t g, uint8_t b, unsigned a ) noexcept
    {
        const int x0 = std::max( rect.x0, 0 );
        const int y0 = std::max( rect.y0, 0 );
        const int x1 = std::min( rect.x1, w );
        const int y1 = std::min( rect.y1, h );
        for ( int y = y0; y < y1; ++y )
        {
            uint8_t* row = rgb + static_cast<size_t>( y ) * stride;
            for ( int x = x0; x < x1; ++x )
                blend_px( row + static_cast<std::ptrdiff_t>( x ) * 3, r, g, b, a );
        }
    }

    // A glyph's padded atlas rect placed in tensor px: tensor (x, y) maps to
    // atlas (ax0 + (x - x0) * inv, ay0 + (y - y0) * inv).
    struct GlyphQuad
    {
        int idx = 0;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // the quad in tensor px
        float ax0 = 0.0f, ay0 = 0.0f;                      // its top-left in the atlas
        float inv = 1.0f;                                  // atlas px per tensor px
    };

    // One tensor row of quad q at atlas row ay: every pixel center inside the
    // quad takes the bilinear alpha (the GL_LINEAR equivalent of the GPU path).
    void draw_glyph_row_cpu( uint8_t* line, int w, const GlyphQuad& q, float ay, uint8_t r, uint8_t g, uint8_t b,
                             unsigned a_mul ) noexcept
    {
        const int px0 = std::max( 0, static_cast<int>( std::floor( q.x0 ) ) );
        const int px1 = std::min( w, static_cast<int>( std::ceil( q.x1 ) ) );
        for ( int px = px0; px < px1; ++px )
        {
            const float cx = static_cast<float>( px ) + 0.5f;
            if ( cx < q.x0 || cx >= q.x1 )
                continue;
            const float cov = glyph_font::sample( q.idx, q.ax0 + ( cx - q.x0 ) * q.inv, ay ) * static_cast<float>( a_mul ) / 255.0f;
            const auto a = static_cast<unsigned>( std::lround( cov ) );
            if ( a != 0u )
                blend_px( line + static_cast<std::ptrdiff_t>( px ) * 3, r, g, b, a );
        }
    }

    // One glyph at float pen position (x0, y0) = its padded rect's top-left in
    // tensor px, scaled by sc: every covered pixel center maps back into the atlas.
    void draw_glyph_cpu( uint8_t* rgb, int w, int h, int stride, float x0, float y0, int glyph_idx, float sc,
                         uint8_t r, uint8_t g, uint8_t b, unsigned a_mul ) noexcept
    {
        const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( glyph_idx );
        if ( m.w == 0 || m.h == 0 || !( sc > 0.0f ) )
            return;
        const GlyphQuad q { glyph_idx,
                            x0,
                            y0,
                            x0 + static_cast<float>( m.w ) * sc,
                            y0 + static_cast<float>( m.h ) * sc,
                            static_cast<float>( m.ax ),
                            static_cast<float>( m.ay ),
                            1.0f / sc };
        const int py0 = std::max( 0, static_cast<int>( std::floor( q.y0 ) ) );
        const int py1 = std::min( h, static_cast<int>( std::ceil( q.y1 ) ) );
        for ( int py = py0; py < py1; ++py )
        {
            const float cy = static_cast<float>( py ) + 0.5f;
            if ( cy < q.y0 || cy >= q.y1 )
                continue;
            draw_glyph_row_cpu( rgb + static_cast<size_t>( py ) * stride, w, q, q.ay0 + ( cy - q.y0 ) * q.inv, r, g, b, a_mul );
        }
    }

    // One text pass of a callout (the CPU twin of append_text_pass in gpu_box_drawer.cpp).
    void draw_text_pass_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutItem& c, float ox, float oy,
                             uint8_t r, uint8_t g, uint8_t b, unsigned a ) noexcept
    {
        float pen = static_cast<float>( c.text_x ) + ox;
        const float top = static_cast<float>( c.text_y ) + oy;
        int pos = 0;
        while ( pos < c.text_bytes )
        {
            const uint32_t cp = glyph_font::next_utf8( c.text, c.text_bytes, pos );
            if ( cp == 0 )
                break;
            const int gi = glyph_font::glyph_index( cp );
            const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( gi );
            draw_glyph_cpu( rgb, w, h, stride, pen + static_cast<float>( m.dx ) * c.scale, top + static_cast<float>( m.dy ) * c.scale,
                            gi, c.scale, r, g, b, a );
            pen += static_cast<float>( m.adv ) * c.scale;
        }
    }

}  // namespace

void draw_boxes_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox* boxes, int n ) noexcept
{
    for ( int i = 0; i < n; ++i )
    {
        const GpuBox& b = boxes[i];
        if ( b.x1 <= 0.0f || b.x0 >= static_cast<float>( w ) || b.y1 <= 0.0f
             || b.y0 >= static_cast<float>( h ) )
            continue;
        if ( b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f )
            continue;
        const int x0 = std::clamp( static_cast<int>( std::floor( b.x0 ) ), 0, w - 1 );
        const int y0 = std::clamp( static_cast<int>( std::floor( b.y0 ) ), 0, h - 1 );
        const int x1 = std::max( x0, std::clamp( static_cast<int>( std::ceil( b.x1 ) ) - 1, 0, w - 1 ) );
        const int y1 = std::max( y0, std::clamp( static_cast<int>( std::ceil( b.y1 ) ) - 1, 0, h - 1 ) );
        const auto put = [&]( int x, int y ) {
            uint8_t* px = rgb + static_cast<size_t>( y ) * stride + static_cast<std::ptrdiff_t>( x ) * 3;
            const unsigned a = b.a;
            if ( a >= 255 )
            {
                px[0] = b.r;
                px[1] = b.g;
                px[2] = b.b;
                return;
            }
            px[0] = static_cast<uint8_t>( ( b.r * a + px[0] * ( 255 - a ) + 127 ) / 255 );
            px[1] = static_cast<uint8_t>( ( b.g * a + px[1] * ( 255 - a ) + 127 ) / 255 );
            px[2] = static_cast<uint8_t>( ( b.b * a + px[2] * ( 255 - a ) + 127 ) / 255 );
        };
        // Each boundary pixel is painted exactly once: Porter-Duff
        // over is not idempotent for a < 255, a double write makes
        // the corner pixels brighter than the edges (trail dots).
        for ( int x = x0; x <= x1; ++x )
        {
            put( x, y0 );
            if ( y1 > y0 )
                put( x, y1 );
        }
        for ( int y = y0 + 1; y < y1; ++y )
        {
            put( x0, y );
            if ( x1 > x0 )
                put( x1, y );
        }
    }
}

// Connector, the 8-way dark outline, the white text: the GPU path's order and colors.
void draw_callouts_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutList& list ) noexcept
{
    for ( int i = 0; i < list.n; ++i )
    {
        const CalloutItem& c = list.d[i];
        for ( int s = 0; s < c.n_segs; ++s )
            fill_rect_cpu( rgb, w, h, stride, c.segs[s], c.r, c.g, c.b, 255u );
        const float o = c.outline;
        const float dirs[8][2] = {
            { -o, -o }, { 0.0f, -o }, { o, -o }, { -o, 0.0f }, { o, 0.0f }, { -o, o }, { 0.0f, o }, { o, o } };
        constexpr uint8_t gray = CalloutConfig::kOutlineGray;
        for ( const auto& d : dirs )
            draw_text_pass_cpu( rgb, w, h, stride, c, d[0], d[1], gray, gray, gray, CalloutConfig::kOutlineAlpha );
        draw_text_pass_cpu( rgb, w, h, stride, c, 0.0f, 0.0f, 255, 255, 255, 255u );
    }
}

