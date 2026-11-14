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
        kTextCap = 64
    };

    // The lib API is char*-based (legacy C): copy into a fixed buffer --
    // const_cast is banned by the project gate (.clang-tidy keeps
    // cppcoreguidelines-pro-type-const-cast on).
    void font_txt( char* dst, int cap, const char* s )
    {
        const int n = static_cast<int>( std::strlen( s ) );
        const int m = n < cap - 1 ? n : cap - 1;
        std::memcpy( dst, s, static_cast<size_t>( m ) );
        dst[m] = 0;
    }
}  // namespace

int BitmapFont::width( const char* s, int scale ) const
{
    if ( !s )
        return 0;
    char txt[kTextCap];
    font_txt( txt, kTextCap, s );
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
        const FontVertex* v = reinterpret_cast<const FontVertex*>( vbuf_ ) + q * 4;
        float min_x = v[0].x;
        float max_x = v[0].x;
        float min_y = v[0].y;
        float max_y = v[0].y;
        for ( int k = 1; k < 4; ++k )
        {
            if ( v[k].x < min_x )
                min_x = v[k].x;
            if ( v[k].x > max_x )
                max_x = v[k].x;
            if ( v[k].y < min_y )
                min_y = v[k].y;
            if ( v[k].y > max_y )
                max_y = v[k].y;
        }
        // Stroke quads carry integer coords, so the scaled fill is
        // exact: [min*scale, max*scale) per axis -- no +1 rounding.
        const int ix0 = x + static_cast<int>( min_x * scale );
        const int iy0 = y + static_cast<int>( min_y * scale );
        int ix1 = x + static_cast<int>( max_x * scale );
        int iy1 = y + static_cast<int>( max_y * scale );
        if ( ix1 > plane.stride )
            ix1 = plane.stride;
        if ( iy1 > plane.h )
            iy1 = plane.h;
        for ( int ry = iy0; ry < iy1; ++ry )
        {
            if ( ry < 0 )
                continue;
            uint8_t* row = plane.p + static_cast<size_t>( ry ) * plane.stride;
            for ( int rx = ix0; rx < ix1; ++rx )
                if ( rx >= 0 )
                    row[rx] = val;
        }
    }
}
