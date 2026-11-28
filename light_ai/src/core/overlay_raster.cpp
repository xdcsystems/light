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

    // The 1-px outline of one box, clipped to the image; w, h > 0, stride >= 3 * w.
    // Non-finite coordinates are skipped, and the span is clamped as float: a cast
    // of NaN, Inf or an out-of-int-range value would be UB.
    void draw_one_box_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox& b ) noexcept
    {
        if ( !std::isfinite( b.x0 ) || !std::isfinite( b.y0 ) || !std::isfinite( b.x1 ) || !std::isfinite( b.y1 ) )
            return;
        if ( b.x1 <= 0.0f || b.x0 >= static_cast<float>( w ) || b.y1 <= 0.0f
             || b.y0 >= static_cast<float>( h ) )
            return;
        if ( b.x1 - b.x0 < 1.0f || b.y1 - b.y0 < 1.0f )
            return;
        const float max_x = static_cast<float>( w - 1 );
        const float max_y = static_cast<float>( h - 1 );
        const int x0 = static_cast<int>( std::clamp( std::floor( b.x0 ), 0.0f, max_x ) );
        const int y0 = static_cast<int>( std::clamp( std::floor( b.y0 ), 0.0f, max_y ) );
        const int x1 = std::max( x0, static_cast<int>( std::clamp( std::ceil( b.x1 ) - 1.0f, 0.0f, max_x ) ) );
        const int y1 = std::max( y0, static_cast<int>( std::clamp( std::ceil( b.y1 ) - 1.0f, 0.0f, max_y ) ) );
        const auto put = [&]( int x, int y ) {
            uint8_t* px = rgb + static_cast<size_t>( y ) * stride + static_cast<std::ptrdiff_t>( x ) * 3;
            blend_px( px, b.r, b.g, b.b, b.a );
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

    // One text pass of a callout (the CPU twin of append_text_pass in gpu_box_drawer.cpp):
    // every covered pixel blends gray at the coverage of one atlas plane (the fill or
    // the baked outline, #108), unrounded until the pixel is written.
    void draw_text_pass_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutItem& c, bool outline, uint8_t gray ) noexcept
    {
        const auto visit = [rgb, stride, outline, gray]( int px, int py, const glyph_font::Texel2& t ) {
            const float cov = ( outline ? t.outline : t.fill ) / 255.0f;
            if ( cov <= 0.0f )
                return;
            uint8_t* p = rgb + static_cast<size_t>( py ) * stride + static_cast<std::ptrdiff_t>( px ) * 3;
            for ( int k = 0; k < 3; ++k )
            {
                const float v = static_cast<float>( gray ) * cov + static_cast<float>( p[k] ) * ( 1.0f - cov );
                p[k] = static_cast<uint8_t>( std::lround( std::clamp( v, 0.0f, 255.0f ) ) );
            }
        };
        glyph_font::raster_text( c.text, c.text_bytes, static_cast<float>( c.text_x ), static_cast<float>( c.text_y ), c.scale, w, h,
                                 visit, true );
    }

}  // namespace

void draw_boxes_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox* boxes, int n ) noexcept
{
    if ( rgb == nullptr || boxes == nullptr || w <= 0 || h <= 0 || static_cast<int64_t>( stride ) < static_cast<int64_t>( w ) * 3
         || n <= 0 )
        return;
    for ( int i = 0; i < n; ++i )
        draw_one_box_cpu( rgb, w, h, stride, boxes[i] );
}

// Connector, the baked dark outline of the whole label, then its white fill: the GPU
// path's order and colors (all outlines under all fills).
void draw_callouts_cpu( uint8_t* rgb, int w, int h, int stride, const CalloutList& list ) noexcept
{
    for ( int i = 0; i < list.n; ++i )
    {
        const CalloutItem& c = list.d[i];
        for ( int s = 0; s < c.n_segs; ++s )
            fill_rect_cpu( rgb, w, h, stride, c.segs[s], c.r, c.g, c.b, 255u );
        draw_text_pass_cpu( rgb, w, h, stride, c, true, CalloutConfig::kOutlineGray );
        draw_text_pass_cpu( rgb, w, h, stride, c, false, 255 );
    }
}
