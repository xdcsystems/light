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
    // The two planes of the (L, A) atlas.
    uint8_t texel( int x, int y )
    {
        return glyph_font::atlas()[( y * glyph_font::kAtlasW + x ) * 2];
    }

    uint8_t outl( int x, int y )
    {
        return glyph_font::atlas()[( y * glyph_font::kAtlasW + x ) * 2 + 1];
    }

    // The outer ring of glyph rect m is 0 in both planes.
    bool border_clear( const glyph_font::GlyphMetric& m )
    {
        bool ok = true;
        for ( int x = m.ax; x < m.ax + m.w; ++x )
            ok = ok && texel( x, m.ay ) == 0u && texel( x, m.ay + m.h - 1 ) == 0u && outl( x, m.ay ) == 0u &&
                 outl( x, m.ay + m.h - 1 ) == 0u;
        for ( int y = m.ay; y < m.ay + m.h; ++y )
            ok = ok && texel( m.ax, y ) == 0u && texel( m.ax + m.w - 1, y ) == 0u && outl( m.ax, y ) == 0u &&
                 outl( m.ax + m.w - 1, y ) == 0u;
        return ok;
    }

    // Some texel of glyph rect m is partly covered (antialiased ink).
    bool has_aa( const glyph_font::GlyphMetric& m )
    {
        for ( int y = m.ay; y < m.ay + m.h; ++y )
            for ( int x = m.ax; x < m.ax + m.w; ++x )
                if ( texel( x, y ) > 0u && texel( x, y ) < 255u )
                    return true;
        return false;
    }

    // Glyph rect i grown by the generator's 2 px gap overlaps no later rect, nor the 4x4 solid block.
    bool rect_apart( int i )
    {
        const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( i );
        const int x0 = m.ax - 2;
        const int y0 = m.ay - 2;
        const int x1 = m.ax + m.w + 2;
        const int y1 = m.ay + m.h + 2;
        bool ok = x0 >= 4 || y0 >= 4;
        for ( int j = i + 1; j < glyph_font::kGlyphCount; ++j )
        {
            const glyph_font::GlyphMetric& n = glyph_font::glyph_metrics( j );
            ok = ok && ( x1 <= n.ax || n.ax + n.w <= x0 || y1 <= n.ay || n.ay + n.h <= y0 );
        }
        return ok;
    }

    // L of glyph rect m at (x, y) relative to its top-left, 0..1; 0 outside the rect.
    float fill_at( const glyph_font::GlyphMetric& m, int x, int y )
    {
        if ( x < 0 || y < 0 || x >= m.w || y >= m.h )
            return 0.0f;
        return static_cast<float>( texel( m.ax + x, m.ay + y ) ) / 255.0f;
    }

    // The A plane of glyph rect m is the exact union of the 8 former outline passes
    // (tools/gen_glyph_font.py): 1 - prod_d (1 - alpha * L(p - d)), d = the 8 offsets of
    // kOutlinePx, alpha = kOutlineAlpha / 255 -- within the 8-bit rounding.
    bool outline_exact( const glyph_font::GlyphMetric& m )
    {
        const float alpha = static_cast<float>( glyph_font::kOutlineAlpha ) / 255.0f;
        const int o = glyph_font::kOutlinePx;
        bool ok = true;
        for ( int y = 0; y < m.h; ++y )
            for ( int x = 0; x < m.w; ++x )
            {
                float keep = 1.0f;
                for ( int dy = -1; dy <= 1; ++dy )
                    for ( int dx = -1; dx <= 1; ++dx )
                        keep *= ( dx == 0 && dy == 0 ) ? 1.0f : 1.0f - alpha * fill_at( m, x - dx * o, y - dy * o );
                const float want = 255.0f * ( 1.0f - keep );
                ok = ok && std::fabs( static_cast<float>( outl( m.ax + x, m.ay + y ) ) - want ) <= 1.0f;
            }
        return ok;
    }

    // The atlas contract of tools/gen_glyph_font.py (#91 item 5, #108): every glyph rect
    // inside the atlas with an outer ring of 0 in both planes, antialiased ink inside, the
    // baked outline exact, and the generator's 2 px gap to every other rect and to the
    // solid block at (0, 0) (L = 255, A = 0) -- GL_LINEAR never reads a neighbour.
    void check_atlas( TestRun& t )
    {
        bool solid = true;
        for ( int y = 0; y < 4; ++y )
            for ( int x = 0; x < 4; ++x )
                solid = solid && texel( x, y ) == 255u && outl( x, y ) == 0u;
        CHECK( t, solid );
        CHECK( t, glyph_font::kSolidX == 2 && glyph_font::kSolidY == 2 );  // the block center: safe for GL_LINEAR
        CHECK( t, glyph_font::kAtlasBytes == glyph_font::kAtlasW * glyph_font::kAtlasH * 2 );
        CHECK( t, glyph_font::kGlyphBorder == glyph_font::kOutlinePx + 1 );
        bool inside = true;
        bool border = true;
        bool aa = true;
        bool apart = true;
        bool exact = true;
        for ( int i = 0; i < glyph_font::kGlyphCount; ++i )
        {
            const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( i );
            inside = inside && m.w >= 2 * glyph_font::kGlyphBorder + 1 && m.h >= 2 * glyph_font::kGlyphBorder + 1 &&
                     m.ax + m.w <= glyph_font::kAtlasW && m.ay + m.h <= glyph_font::kAtlasH;
            if ( !inside )
                break;
            border = border && border_clear( m );
            aa = aa && has_aa( m );
            apart = apart && rect_apart( i );
            exact = exact && outline_exact( m );
        }
        CHECK( t, inside );
        CHECK( t, border );
        CHECK( t, aa );
        CHECK( t, apart );
        CHECK( t, exact );
        // The line box holds every glyph's ink (the padded rect sticks out by the border).
        bool in_line = true;
        for ( int i = 0; i < glyph_font::kGlyphCount; ++i )
        {
            const glyph_font::GlyphMetric& m = glyph_font::glyph_metrics( i );
            in_line = in_line && m.dy + glyph_font::kGlyphBorder >= 0 &&
                      m.dy + m.h - glyph_font::kGlyphBorder <= glyph_font::kLineH;
        }
        CHECK( t, in_line );
    }

    // glyph_font::sample2: bilinear in both planes, clamped to the glyph's own rect.
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
        const glyph_font::Texel2 c = glyph_font::sample2( gi, cx, cy );  // a texel center: the texel itself
        CHECK_NEAR( t, c.fill, 255.0f, 1e-3f );
        CHECK_NEAR( t, c.outline, static_cast<float>( outl( ox, oy ) ), 1e-3f );
        const float mid = 0.5f * ( static_cast<float>( texel( ox, oy ) ) + static_cast<float>( texel( ox + 1, oy ) ) );
        const float mid_o = 0.5f * ( static_cast<float>( outl( ox, oy ) ) + static_cast<float>( outl( ox + 1, oy ) ) );
        const glyph_font::Texel2 h = glyph_font::sample2( gi, cx + 0.5f, cy );  // halfway: the mean of two
        CHECK_NEAR( t, h.fill, mid, 1e-3f );
        CHECK_NEAR( t, h.outline, mid_o, 1e-3f );
        // The outline reaches kOutlinePx texels left of the first ink texel of the row, the fill does not.
        const glyph_font::Texel2 left = glyph_font::sample2( gi, cx - static_cast<float>( glyph_font::kOutlinePx ), cy );
        CHECK( t, left.outline > 0.0f && left.fill == 0.0f );
        const glyph_font::Texel2 far = glyph_font::sample2( gi, static_cast<float>( m.ax ) - 50.0f, cy );  // clamped to the ring
        CHECK( t, far.fill == 0.0f && far.outline == 0.0f );
        const glyph_font::Texel2 sp = glyph_font::sample2( glyph_font::kGlyphSpace, cx, cy );  // the space has no ink
        CHECK( t, sp.fill == 0.0f && sp.outline == 0.0f );
        const glyph_font::Texel2 nan = glyph_font::sample2( gi, std::nanf( "" ), cy );
        CHECK( t, nan.fill == 0.0f && nan.outline == 0.0f );
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

    // The glyph walk (for_each_glyph) and the CPU raster on it (raster_text).
    void check_walk( TestRun& t )
    {
        // "A A": two glyphs (the space only advances), the second one pen + adv(A) + adv(space) later.
        int n = 0;
        float first_x = 0.0f;
        float last_x = 0.0f;
        float last_y = 0.0f;
        glyph_font::for_each_glyph( "A A", 3, 10.0f, 20.0f, 0.5f,
                                    [&]( int gi, const glyph_font::GlyphMetric& m, float gx, float gy ) {
                                        first_x = n == 0 ? gx : first_x;
                                        last_x = gx;
                                        last_y = gy;
                                        n += gi == glyph_font::glyph_index( 'A' ) && m.w > 0 ? 1 : 100;
                                    } );
        const glyph_font::GlyphMetric& a = glyph_font::glyph_metrics( glyph_font::glyph_index( 'A' ) );
        CHECK_EQ( t, n, 2 );
        CHECK_NEAR( t, first_x, 10.0f + static_cast<float>( a.dx ) * 0.5f, 1e-5f );
        CHECK_NEAR( t, last_x - first_x, static_cast<float>( a.adv + glyph_font::kSpaceAdv ) * 0.5f, 1e-5f );
        CHECK_NEAR( t, last_y, 20.0f + static_cast<float>( a.dy ) * 0.5f, 1e-5f );

        // raster_text visits only pixels of the target, inside the glyph quads, and finds ink.
        int visits = 0;
        int outside = 0;
        float max_fill = 0.0f;
        glyph_font::raster_text( "H", 1, -3.0f, -2.0f, 1.0f, 8, 6, [&]( int px, int py, const glyph_font::Texel2& s ) {
            ++visits;
            outside += px < 0 || py < 0 || px >= 8 || py >= 6 ? 1 : 0;
            max_fill = s.fill > max_fill ? s.fill : max_fill;
        } );
        CHECK( t, visits > 0 && outside == 0 && max_fill > 200.0f );
        int none = 0;
        glyph_font::raster_text( "H", 1, 0.0f, 0.0f, std::nanf( "" ), 8, 6, [&]( int, int, const glyph_font::Texel2& ) { ++none; } );
        glyph_font::raster_text( "H", 1, 0.0f, 0.0f, 0.0f, 8, 6, [&]( int, int, const glyph_font::Texel2& ) { ++none; } );
        glyph_font::raster_text( "H", 1, std::nanf( "" ), 0.0f, 1.0f, 8, 6, [&]( int, int, const glyph_font::Texel2& ) { ++none; } );
        glyph_font::raster_text( "H", 1, 0.0f, 1e30f, 1.0f, 8, 6, [&]( int, int, const glyph_font::Texel2& ) { ++none; } );
        CHECK_EQ( t, none, 0 );
        // A huge finite scale: the quad covers the whole target, every visit stays inside it.
        int huge_out = 0;
        glyph_font::raster_text( "H", 1, -1e20f, -1e20f, 1e30f, 8, 6, [&]( int px, int py, const glyph_font::Texel2& ) {
            huge_out += px < 0 || py < 0 || px >= 8 || py >= 6 ? 1 : 0;
        } );
        CHECK_EQ( t, huge_out, 0 );
    }
}  // namespace

void test_glyph_font( TestRun& t )
{
    check_atlas( t );
    check_sample( t );
    check_walk( t );

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

    // Proportional metrics at the 22 px atlas: 'Person #7' is 113 atlas px (sum of
    // the advances), 56.5 px at the 11 px default.
    CHECK( t, glyph_font::kAtlasPx == 22 );
    CHECK( t, glyph_font::text_width( "Person #7" ) == 113 );
}
