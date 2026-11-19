#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "check.hpp"
#include "core/glyph_font.hpp"
#include "suites.hpp"

namespace
{
    uint8_t texel( int x, int y )
    {
        return glyph_font::atlas()[y * glyph_font::kAtlasW + x];
    }

    // The atlas contract of tools/gen_glyph_font.py (#91 item 5): every glyph rect
    // inside the atlas with a transparent 1 px border, antialiased ink inside, and
    // the generator's 2 px gap to every other rect and to the opaque solid block at
    // (0, 0) -- GL_LINEAR never reads a neighbour.
    void check_atlas( TestRun& t )
    {
        bool solid = true;
        for ( int y = 0; y < 4; ++y )
            for ( int x = 0; x < 4; ++x )
                solid = solid && texel( x, y ) == 255u;
        CHECK( t, solid );
        CHECK( t, glyph_font::kSolidX == 2 && glyph_font::kSolidY == 2 );  // the block center: safe for GL_LINEAR
        bool inside = true;
        bool border = true;
        bool aa = true;
        bool apart = true;
        for ( int i = 0; i < glyph_font::kGlyphCount; ++i )
        {
            const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( i );
            inside = inside && m.w >= 3 && m.h >= 3 && m.ax + m.w <= glyph_font::kAtlasW && m.ay + m.h <= glyph_font::kAtlasH;
            if ( !inside )
                break;
            bool has_aa = false;
            for ( int x = m.ax; x < m.ax + m.w; ++x )
                border = border && texel( x, m.ay ) == 0u && texel( x, m.ay + m.h - 1 ) == 0u;
            for ( int y = m.ay; y < m.ay + m.h; ++y )
            {
                border = border && texel( m.ax, y ) == 0u && texel( m.ax + m.w - 1, y ) == 0u;
                for ( int x = m.ax; x < m.ax + m.w; ++x )
                    has_aa = has_aa || ( texel( x, y ) > 0u && texel( x, y ) < 255u );
            }
            aa = aa && has_aa;
            // Rect i grown by the 2 px gap must not overlap rect j, nor the solid block.
            const int x0 = m.ax - 2;
            const int y0 = m.ay - 2;
            const int x1 = m.ax + m.w + 2;
            const int y1 = m.ay + m.h + 2;
            apart = apart && ( x0 >= 4 || y0 >= 4 );
            for ( int j = i + 1; j < glyph_font::kGlyphCount; ++j )
            {
                const glyph_font::GlyphMetric& n = glyph_font::glyph_metrics( j );
                apart = apart && ( x1 <= n.ax || n.ax + n.w <= x0 || y1 <= n.ay || n.ay + n.h <= y0 );
            }
        }
        CHECK( t, inside );
        CHECK( t, border );
        CHECK( t, aa );
        CHECK( t, apart );
        // The line box holds every glyph (dy, dy + h within [-1, kLineH + 1]: the border may stick out by 1 px).
        bool in_line = true;
        for ( int i = 0; i < glyph_font::kGlyphCount; ++i )
        {
            const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( i );
            in_line = in_line && m.dy >= -1 && m.dy + m.h <= glyph_font::kLineH + 1;
        }
        CHECK( t, in_line );
    }

    // glyph_font::sample: bilinear, clamped to the glyph's own rect.
    void check_sample( TestRun& t )
    {
        const int gi = glyph_font::glyph_index( 'H' );
        const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( gi );
        int ox = -1;
        int oy = -1;
        for ( int y = m.ay; y < m.ay + m.h && ox < 0; ++y )
            for ( int x = m.ax; x < m.ax + m.w && ox < 0; ++x )
                if ( texel( x, y ) == 255u )
                {
                    ox = x;
                    oy = y;
                }
        CHECK( t, ox >= 0 );
        if ( ox < 0 )
            return;
        const float cx = static_cast<float>( ox ) + 0.5f;
        const float cy = static_cast<float>( oy ) + 0.5f;
        CHECK_NEAR( t, glyph_font::sample( gi, cx, cy ), 255.0f, 1e-3f );  // a texel center: the texel itself
        const float mid = 0.5f * ( static_cast<float>( texel( ox, oy ) ) + static_cast<float>( texel( ox + 1, oy ) ) );
        CHECK_NEAR( t, glyph_font::sample( gi, cx + 0.5f, cy ), mid, 1e-3f );  // halfway: the mean of two
        CHECK_NEAR( t, glyph_font::sample( gi, static_cast<float>( m.ax ) - 50.0f, cy ), 0.0f, 1e-3f );  // clamped to the border
        CHECK_NEAR( t, glyph_font::sample( glyph_font::kGlyphSpace, cx, cy ), 0.0f, 1e-6f );             // the space has no ink
        CHECK_NEAR( t, glyph_font::sample( gi, std::nanf( "" ), cy ), 0.0f, 1e-6f );
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        glyph_font::glyph_uv( gi, u0, v0, u1, v1 );
        CHECK_NEAR( t, u0 * static_cast<float>( glyph_font::kAtlasW ), static_cast<float>( m.ax ), 1e-3f );
        CHECK_NEAR( t, v1 * static_cast<float>( glyph_font::kAtlasH ), static_cast<float>( m.ay + m.h ), 1e-3f );
        glyph_font::glyph_uv( glyph_font::kGlyphSpace, u0, v0, u1, v1 );
        CHECK( t, u0 == 0.0f && u1 == 0.0f && v0 == 0.0f && v1 == 0.0f );
    }
}  // namespace

void test_glyph_font( TestRun& t )
{
    check_atlas( t );
    check_sample( t );

    // Every Cyrillic codepoint A..Ya, a..ya, Yo, yo maps to a real glyph
    // distinct from the '?' fallback; Yo and yo are distinct glyphs.
    const std::string sample = "\xD1\x87\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA 42 \xD0\x81\xD1\x91 \xD0\xAF";
    CHECK( t, glyph_font::text_len( sample ) == 15 );
    int pos = 0;
    const int bytes = static_cast<int>( sample.size() );
    CHECK( t, glyph_font::next_utf8( sample.data(), bytes, pos ) == 0x0447u );
    const int unknown_ref = glyph_font::glyph_index( 0x0001u );  // '?' fallback index
    CHECK( t, unknown_ref == glyph_font::glyph_index( 0x10FFFFu ) );  // unknowns share the fallback
    bool cyr_ok = true;
    for ( const uint32_t cp : { 0x0410u, 0x044Fu, 0x0401u, 0x0451u, 0x0447u, 0x042Fu } )
        cyr_ok = cyr_ok && ( glyph_font::glyph_index( cp ) != unknown_ref );
    CHECK( t, cyr_ok );
    CHECK( t, glyph_font::glyph_index( 0x0401u ) != glyph_font::glyph_index( 0x0451u ) );

    // Malformed, truncated, overlong, surrogate, and control UTF-8 decode safely without OOB.
    const char bad[] = {
        static_cast<char>( 0xD0 ),  // truncated 2-byte at end
    };
    int bpos = 0;
    CHECK( t, glyph_font::next_utf8( bad, 1, bpos ) == '?' && bpos == 1 );

    const char mixed[] = {
        static_cast<char>( 0xC0 ), static_cast<char>( 0xAF ),                              // overlong '/'
        static_cast<char>( 0xED ), static_cast<char>( 0xA0 ), static_cast<char>( 0x80 ),   // UTF-16 surrogate U+D800
        static_cast<char>( 0x80 ),                                                         // orphan continuation
        '\n',
        'Z',
    };
    int mpos = 0;
    const int mlen = static_cast<int>( sizeof( mixed ) );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );   // overlong
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );   // surrogate rejected
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );   // orphan continuation
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '\n' );  // control char passes through
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == 'Z' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == 0 );

    // format_person: zero-heap formatting for En and Ru + INT_MIN edge.
    char buf[glyph_font::kLabelCap] {};
    const int n_en = glyph_font::format_person( buf, glyph_font::kLabelCap, 7, false );
    CHECK( t, n_en == 9 && std::strcmp( buf, "Person #7" ) == 0 );
    const int n_ru = glyph_font::format_person( buf, glyph_font::kLabelCap, 12, true );
    CHECK( t, std::strcmp( buf, "\xD0\xA7\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA #12" ) == 0 );
    CHECK( t, glyph_font::text_len( std::string_view( buf, static_cast<std::size_t>( n_ru ) ) ) == 11 );
    const int n_min = glyph_font::format_person( buf, glyph_font::kLabelCap, INT_MIN, true );
    CHECK( t, n_min > 15 && buf[n_min] == '\0' );

    // Proportional metrics at the 32 px atlas: 'Person #7' is 166 atlas px (sum of
    // the advances), 83 px at the 16 px default -- the width of the former 16 px table.
    CHECK( t, glyph_font::kAtlasPx == 32 );
    CHECK( t, glyph_font::text_width( "Person #7" ) == 166 );
}
