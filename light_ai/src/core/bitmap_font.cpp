#include <algorithm>
#include <cstddef>
#include <cstring>

// The one translation unit that includes the library (its file statics stay here).
#include <stb_easy_font.h>

#include "core/bitmap_font.hpp"

namespace
{
    // One stb_easy_font vertex: x/y/z float + color uint8[4], 16 bytes,
    // four per quad.
    struct FontVertex
    {
        float x;
        float y;
        float z;
        unsigned char c[4];
    };

    enum
    {
        kTextCap = 32
    };

    // The lib API is char*-based (legacy C): copy into a fixed buffer --
    // const_cast is banned by the project gate (.clang-tidy keeps
    // cppcoreguidelines-pro-type-const-cast on).
    void font_txt( char* dst, int cap, const char* s )
    {
        if ( cap <= 0 )
            return;
        int i = 0;
        for ( ; i < cap - 1 && s[i] != '\0'; ++i )
        {
            const unsigned char uc = static_cast<unsigned char>( s[i] );
            dst[i] = ( uc == '\n' || ( uc >= 32 && uc <= 126 ) ) ? static_cast<char>( uc ) : ' ';
        }
        dst[i] = '\0';
    }

    // A pixel rectangle [x0, x1) x [y0, y1), already clipped to the plane
    // (empty when x0 >= x1 or y0 >= y1).
    struct QuadBox
    {
        int x0;
        int y0;
        int x1;
        int y1;
    };

    // The bbox of one stroke quad (4 vertices), scaled, placed at (x, y) and
    // clipped to the plane. Stroke quads carry integer coords, so the scaled
    // fill is exact: [min*scale, max*scale) per axis -- no +1 rounding.
    QuadBox quad_box( const FontVertex* v, int x, int y, int scale, const Plane8& plane )
    {
        float min_x = v[0].x;
        float max_x = v[0].x;
        float min_y = v[0].y;
        float max_y = v[0].y;
        for ( int k = 1; k < 4; ++k )
        {
            min_x = std::min( min_x, v[k].x );
            max_x = std::max( max_x, v[k].x );
            min_y = std::min( min_y, v[k].y );
            max_y = std::max( max_y, v[k].y );
        }
        const int x0 = x + static_cast<int>( min_x * scale );
        const int y0 = y + static_cast<int>( min_y * scale );
        const int x1 = x + static_cast<int>( max_x * scale );
        const int y1 = y + static_cast<int>( max_y * scale );
        return QuadBox { std::max( x0, 0 ), std::max( y0, 0 ), std::min( x1, plane.stride ), std::min( y1, plane.h ) };
    }

    void fill_box( const Plane8& plane, const QuadBox& b, uint8_t val )
    {
        for ( int ry = b.y0; ry < b.y1; ++ry )
        {
            uint8_t* row = plane.p + static_cast<size_t>( ry ) * plane.stride;
            for ( int rx = b.x0; rx < b.x1; ++rx )
                row[rx] = val;
        }
    }
}  // namespace

int BitmapFont::width( const char* s, int scale ) const
{
    if ( !s )
        return 0;
    char txt[kTextCap];
    font_txt( txt, kTextCap, s );
    stb_easy_font_spacing( 0 );  // match stamp() default spacing
    return stb_easy_font_width( txt ) * scale;
}

int BitmapFont::height( const char* s, int scale ) const
{
    if ( !s )
        return 0;
    char txt[kTextCap];
    font_txt( txt, kTextCap, s );
    return stb_easy_font_height( txt ) * scale;
}

void BitmapFont::stamp( const Plane8& plane, int x, int y, const char* s, int scale, uint8_t val )
{
    if ( !plane.p || scale <= 0 || !s )
        return;
    char txt[kTextCap];
    font_txt( txt, kTextCap, s );
    stb_easy_font_spacing( 0 );  // default spacing; keeps the static referenced
    unsigned char col[4] = { val, val, val, 255 };
    const int nq = stb_easy_font_print( 0.0f, 0.0f, txt, col, vbuf_, static_cast<int>( sizeof( vbuf_ ) ) );
    for ( int q = 0; q < nq; ++q )
    {
        const FontVertex* v = reinterpret_cast<const FontVertex*>( vbuf_ ) + static_cast<std::ptrdiff_t>( q ) * 4;
        fill_box( plane, quad_box( v, x, y, scale, plane ), val );
    }
}
