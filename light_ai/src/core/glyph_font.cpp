// The label font functions on top of the generated atlas (glyph_font_table.hpp,
// tools/gen_glyph_font.py). No state: everything reads the constexpr tables.

#include <algorithm>
#include <cmath>

#include "core/glyph_font.hpp"
#include "core/glyph_font_table.hpp"

namespace glyph_font
{
    namespace
    {
        constexpr GlyphMetric kSpaceMetric = { 0, 0, 0, 0, 0, 0, kSpaceAdv };

        constexpr int question_index() noexcept
        {
            for ( int i = 0; i < kGlyphCount; ++i )
            {
                if ( kCodepoints[i] == 0x3Fu )
                    return i;
            }
            return -1;
        }
        constexpr int kGlyphIndexQuestion = question_index();
        static_assert( kGlyphIndexQuestion >= 0, "the charset has no '?' fallback" );

        const GlyphMetric& metric_at( int idx ) noexcept
        {
            if ( idx == kGlyphSpace )
                return kSpaceMetric;
            if ( idx < 0 || idx >= kGlyphCount )
                return kGlyphMetrics[kGlyphIndexQuestion];
            return kGlyphMetrics[idx];
        }
    }  // namespace

    int glyph_index( uint32_t cp ) noexcept
    {
        if ( cp == 0x20u )
            return kGlyphSpace;
        for ( int i = 0; i < kGlyphCount; ++i )
        {
            if ( kCodepoints[i] == cp )
                return i;
        }
        return kGlyphIndexQuestion;
    }

    // A UTF-8 lead byte (not ASCII): the continuation bytes it announces and its payload bits in cp; 0 for a
    // continuation byte or an invalid lead.
    int utf8_lead( uint32_t c0, uint32_t& cp ) noexcept
    {
        if ( ( c0 & 0xE0u ) == 0xC0u )
        {
            cp = c0 & 0x1Fu;
            return 1;
        }
        if ( ( c0 & 0xF0u ) == 0xE0u )
        {
            cp = c0 & 0x0Fu;
            return 2;
        }
        if ( ( c0 & 0xF8u ) == 0xF0u )
        {
            cp = c0 & 0x07u;
            return 3;
        }
        return 0;
    }

    uint32_t next_utf8( const char* str, int len, int& pos ) noexcept
    {
        if ( pos >= len )
            return 0;
        const auto c0 = static_cast<uint8_t>( str[pos] );
        if ( c0 < 0x80u )
        {
            ++pos;
            return c0;
        }

        uint32_t cp = 0;
        const int need = utf8_lead( c0, cp );

        if ( need == 0 )
        {
            ++pos;
            return '?';
        }
        if ( pos + need >= len )
        {
            pos = len;
            return '?';
        }
        for ( int i = 1; i <= need; ++i )
        {
            const auto cc = static_cast<uint8_t>( str[pos + i] );
            if ( ( cc & 0xC0u ) != 0x80u )
            {
                ++pos;
                return '?';
            }
            cp = ( cp << 6 ) | ( cc & 0x3Fu );
        }
        pos += need + 1;

        constexpr uint32_t k_min_cp[4] = { 0, 0x80u, 0x800u, 0x10000u };
        if ( cp < k_min_cp[need] || cp > 0x10FFFFu || ( cp >= 0xD800u && cp <= 0xDFFFu ) )
            return '?';
        return cp;
    }

    int text_len( std::string_view text ) noexcept
    {
        const int len = static_cast<int>( text.size() );

        int pos = 0;
        int count = 0;

        while ( pos < len )
        {
            const uint32_t cp = next_utf8( text.data(), len, pos );
            if ( cp == 0 )
                break;
            ++count;
        }

        return count;
    }

    const GlyphMetric& glyph_metrics( int idx ) noexcept { return metric_at( idx ); }

    void glyph_uv( int idx, float& u0, float& v0, float& u1, float& v1 ) noexcept
    {
        const GlyphMetric& m = metric_at( idx );
        if ( m.w == 0 || m.h == 0 )
        {
            u0 = 0.0f;
            v0 = 0.0f;
            u1 = 0.0f;
            v1 = 0.0f;
            return;
        }
        u0 = static_cast<float>( m.ax ) / static_cast<float>( kAtlasW );
        v0 = static_cast<float>( m.ay ) / static_cast<float>( kAtlasH );
        u1 = static_cast<float>( m.ax + m.w ) / static_cast<float>( kAtlasW );
        v1 = static_cast<float>( m.ay + m.h ) / static_cast<float>( kAtlasH );
    }

    void solid_uv( float& u, float& v ) noexcept
    {
        u = static_cast<float>( kSolidX ) / static_cast<float>( kAtlasW );
        v = static_cast<float>( kSolidY ) / static_cast<float>( kAtlasH );
    }

    const uint8_t* atlas() noexcept { return kAtlas; }

    float sample( int idx, float x, float y ) noexcept
    {
        const GlyphMetric& m = metric_at( idx );
        if ( m.w == 0 || m.h == 0 || !std::isfinite( x ) || !std::isfinite( y ) )
            return 0.0f;
        // Texel centers sit at +0.5: shift to center coordinates, clamp into the rect.
        const float fx = std::clamp( x - 0.5f, static_cast<float>( m.ax ), static_cast<float>( m.ax + m.w - 1 ) );
        const float fy = std::clamp( y - 0.5f, static_cast<float>( m.ay ), static_cast<float>( m.ay + m.h - 1 ) );
        const int x0 = static_cast<int>( fx );
        const int y0 = static_cast<int>( fy );
        const int x1 = std::min( x0 + 1, m.ax + m.w - 1 );
        const int y1 = std::min( y0 + 1, m.ay + m.h - 1 );
        const float tx = fx - static_cast<float>( x0 );
        const float ty = fy - static_cast<float>( y0 );
        const auto at = []( int px, int py ) {
            return static_cast<float>( kAtlas[py * kAtlasW + px] );
        };
        const float top = at( x0, y0 ) + ( at( x1, y0 ) - at( x0, y0 ) ) * tx;
        const float bot = at( x0, y1 ) + ( at( x1, y1 ) - at( x0, y1 ) ) * tx;
        return top + ( bot - top ) * ty;
    }

    int format_person( char* dst, int dst_cap, int id, bool lang_ru ) noexcept
    {
        if ( dst == nullptr || dst_cap <= 0 )
            return 0;
        int n = 0;
        const auto put = [&dst, &n, dst_cap]( char c ) {
            if ( n + 1 < dst_cap )
                dst[n++] = c;
        };
        if ( lang_ru )
        {
            for ( const char* p = "Человек #"; *p != 0; ++p )
                put( *p );
        }
        else
        {
            for ( const char* p = "Person #"; *p != 0; ++p )
                put( *p );
        }
        if ( id < 0 )
            id = 0;
        char digits[10];
        int d = 0;
        do
        {
            digits[d++] = static_cast<char>( '0' + id % 10 );
            id /= 10;
        }
        while ( id != 0 && d < 10 );
        while ( d > 0 )
            put( digits[--d] );
        dst[n] = 0;
        return n;
    }

    int text_width( std::string_view text ) noexcept
    {
        const int len = static_cast<int>( text.size() );

        int pos = 0;
        int w = 0;

        while ( pos < len )
        {
            const uint32_t cp = next_utf8( text.data(), len, pos );
            if ( cp == 0 )
                break;
            w += metric_at( glyph_index( cp ) ).adv;
        }

        return w;
    }

}  // namespace glyph_font
