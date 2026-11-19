#include <array>
#include <climits>
#include <cstring>
#include <string>

#include "check.hpp"
#include "core/glyph_font.hpp"
#include "suites.hpp"

void test_glyph_font( TestRun& t )
{
    // Guaranteed-opaque texel inside glyph 'f' (used by GPU shader for solid quads).
    std::array<uint8_t, glyph_font::kAtlasBytes> atlas {};
    glyph_font::build_atlas( atlas );
    const int solid_off = glyph_font::kSolidY * glyph_font::kAtlasW + glyph_font::kSolidX;
    CHECK( t, atlas[solid_off] == 0xFFu );

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

    // Atlas row for glyph 'A' matches glyph_alpha() cell; AA intermediates present; cell corners clear.
    const int idx_a = glyph_font::glyph_index( 'A' );
    const uint8_t* alpha_a = glyph_font::glyph_alpha( idx_a );
    bool match_a = true;
    bool has_aa = false;
    for ( int r = 0; r < glyph_font::kGlyphH; ++r )
    {
        for ( int c = 0; c < glyph_font::kGlyphWMax; ++c )
        {
            const uint8_t got = atlas[( idx_a * glyph_font::kGlyphH + r ) * glyph_font::kAtlasW + c];
            const uint8_t exp = alpha_a[r * glyph_font::kGlyphWMax + c];
            match_a = match_a && ( got == exp );
            has_aa = has_aa || ( got > 0u && got < 255u );
        }
    }
    const int cell0 = idx_a * glyph_font::kGlyphH * glyph_font::kAtlasW;
    const bool border_zero = ( atlas[cell0] == 0u ) &&
                             ( atlas[( idx_a * glyph_font::kGlyphH + glyph_font::kGlyphH - 1 ) * glyph_font::kAtlasW] == 0u );
    CHECK( t, match_a && has_aa && border_zero );

    // format_person: zero-heap formatting for En and Ru + INT_MIN edge.
    char buf[glyph_font::kLabelCap] {};
    const int n_en = glyph_font::format_person( buf, glyph_font::kLabelCap, 7, false );
    CHECK( t, n_en == 9 && std::strcmp( buf, "Person #7" ) == 0 );
    const int n_ru = glyph_font::format_person( buf, glyph_font::kLabelCap, 12, true );
    CHECK( t, std::strcmp( buf, "\xD0\xA7\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA #12" ) == 0 );
    CHECK( t, glyph_font::text_len( std::string_view( buf, static_cast<std::size_t>( n_ru ) ) ) == 11 );
    const int n_min = glyph_font::format_person( buf, glyph_font::kLabelCap, INT_MIN, true );
    CHECK( t, n_min > 15 && buf[n_min] == '\0' );

    // Proportional metrics: 'Person #7' occupies 83 px (sum of per-glyph advances).
    CHECK( t, glyph_font::text_width( "Person #7" ) == 83 );
}
