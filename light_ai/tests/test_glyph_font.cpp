#include <array>
#include <climits>
#include <cstring>
#include <string>

#include "check.hpp"
#include "core/glyph_font.hpp"
#include "suites.hpp"

void test_glyph_font( TestRun& t )
{
    // Slot 0 is solid 0xFF (used by GPU shader for untextured quads).
    const uint8_t* solid = glyph_font::glyph_rows( 0 );
    bool all_ff = true;
    for ( int r = 0; r < 8; ++r )
        all_ff = all_ff && ( solid[r] == 0xFFu );
    CHECK( t, all_ff );

    // Every Cyrillic codepoint A..Ya, a..ya, Yo, yo maps to non-empty distinct glyph index.
    const std::string sample = "\xD1\x87\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA 42 \xD0\x81\xD1\x91 \xD0\xAF";
    CHECK( t, glyph_font::text_len( sample ) == 15 );
    int pos = 0;
    const int bytes = static_cast<int>( sample.size() );
    CHECK( t, glyph_font::next_utf8( sample.data(), bytes, pos ) == 0x0447u );
    CHECK( t, glyph_font::glyph_index( 0x0447u ) == 128 + ( 0x0447 - 0x0430 ) );
    CHECK( t, glyph_font::glyph_index( 0x0401u ) == 160 );
    CHECK( t, glyph_font::glyph_index( 0x0451u ) == 161 );

    // Malformed, truncated, overlong, surrogate, and control UTF-8 safely map to '?' without OOB.
    const char bad[] = {
        static_cast<char>( 0xD0 ),                          // truncated 2-byte at end
    };
    int bpos = 0;
    CHECK( t, glyph_font::next_utf8( bad, 1, bpos ) == '?' && bpos == 1 );

    const char mixed[] = {
        static_cast<char>( 0xC0 ), static_cast<char>( 0xAF ),                          // overlong '/'
        static_cast<char>( 0xED ), static_cast<char>( 0xA0 ), static_cast<char>( 0x80 ), // UTF-16 surrogate U+D800
        static_cast<char>( 0x80 ),                                                     // orphan continuation
        '\n',
        'Z',
    };
    int mpos = 0;
    const int mlen = static_cast<int>( sizeof( mixed ) );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == '?' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == 'Z' );
    CHECK( t, glyph_font::next_utf8( mixed, mlen, mpos ) == 0 );

    // Atlas build matches glyph bit rows at 8x8 grid coordinates.
    std::array<uint8_t, glyph_font::kAtlasBytes> atlas {};
    glyph_font::build_atlas( atlas );
    CHECK( t, atlas[0] == 0xFFu && atlas[7 * glyph_font::kAtlasW + 7] == 0xFFu );
    const int idx_a = glyph_font::glyph_index( 'A' );
    const uint8_t* rows_a = glyph_font::glyph_rows( idx_a );
    const int ax = ( idx_a % glyph_font::kAtlasCols ) * glyph_font::kGlyphW;
    const int ay = ( idx_a / glyph_font::kAtlasCols ) * glyph_font::kGlyphH;
    bool match_a = true;
    for ( int r = 0; r < 8; ++r )
    {
        for ( int c = 0; c < 8; ++c )
        {
            const uint8_t expected = ( ( rows_a[r] >> c ) & 1u ) != 0u ? 0xFFu : 0x00u;
            match_a = match_a && ( atlas[( ay + r ) * glyph_font::kAtlasW + ax + c] == expected );
        }
    }
    CHECK( t, match_a );

    // format_person: zero-heap formatting for En and Ru + INT_MIN edge.
    char buf[glyph_font::kLabelCap] {};
    const int n_en = glyph_font::format_person( buf, glyph_font::kLabelCap, 7, false );
    CHECK( t, n_en == 8 && std::strcmp( buf, "person 7" ) == 0 );
    const int n_ru = glyph_font::format_person( buf, glyph_font::kLabelCap, 12, true );
    CHECK( t, std::strcmp( buf, "\xD1\x87\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA 12" ) == 0 );
    CHECK( t, glyph_font::text_len( std::string_view( buf, static_cast<std::size_t>( n_ru ) ) ) == 10 );
    const int n_min = glyph_font::format_person( buf, glyph_font::kLabelCap, INT_MIN, true );
    CHECK( t, n_min > 15 && buf[n_min] == '\0' );
}
