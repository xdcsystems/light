// Proportional DejaVu Sans font implementation on top of the generated atlas
// in glyph_font_table.hpp (147 glyphs, 18x18 cells, single-column strip).
// Replaces the former hand-drawn table: glyphs come from the generated table.

#include <cstring>

#include "core/glyph_font_table.hpp"
#include "core/glyph_font.hpp"

namespace glyph_font
{
    namespace
    {
        // Codepoints in exact atlas order; mirrors kGlyphChars from the table.
        // Виртуальный глиф пробела и solid-тексель для рамок
        // constexpr int kGlyphSpace = kGlyphCount;
        // constexpr int kSpaceAdv = 5;
        // constexpr int kSolidX = 2;
        // constexpr int kSolidY = 744;

constexpr uint32_t kCodepoints[ kGlyphCount ] = {
            0x0030u, 0x0031u, 0x0032u, 0x0033u, 0x0034u, 0x0035u,
            0x0036u, 0x0037u, 0x0038u, 0x0039u, 0x0041u, 0x0042u,
            0x0043u, 0x0044u, 0x0045u, 0x0046u, 0x0047u, 0x0048u,
            0x0049u, 0x004Au, 0x004Bu, 0x004Cu, 0x004Du, 0x004Eu,
            0x004Fu, 0x0050u, 0x0051u, 0x0052u, 0x0053u, 0x0054u,
            0x0055u, 0x0056u, 0x0057u, 0x0058u, 0x0059u, 0x005Au,
            0x0061u, 0x0062u, 0x0063u, 0x0064u, 0x0065u, 0x0066u,
            0x0067u, 0x0068u, 0x0069u, 0x006Au, 0x006Bu, 0x006Cu,
            0x006Du, 0x006Eu, 0x006Fu, 0x0070u, 0x0071u, 0x0072u,
            0x0073u, 0x0074u, 0x0075u, 0x0076u, 0x0077u, 0x0078u,
            0x0079u, 0x007Au, 0x0410u, 0x0411u, 0x0412u, 0x0413u,
            0x0414u, 0x0415u, 0x0401u, 0x0416u, 0x0417u, 0x0418u,
            0x0419u, 0x041Au, 0x041Bu, 0x041Cu, 0x041Du, 0x041Eu,
            0x041Fu, 0x0420u, 0x0421u, 0x0422u, 0x0423u, 0x0424u,
            0x0425u, 0x0426u, 0x0427u, 0x0428u, 0x0429u, 0x042Au,
            0x042Bu, 0x042Cu, 0x042Du, 0x042Eu, 0x042Fu, 0x0430u,
            0x0431u, 0x0432u, 0x0433u, 0x0434u, 0x0435u, 0x0451u,
            0x0436u, 0x0437u, 0x0438u, 0x0439u, 0x043Au, 0x043Bu,
            0x043Cu, 0x043Du, 0x043Eu, 0x043Fu, 0x0440u, 0x0441u,
            0x0442u, 0x0443u, 0x0444u, 0x0445u, 0x0446u, 0x0447u,
            0x0448u, 0x0449u, 0x044Au, 0x044Bu, 0x044Cu, 0x044Du,
            0x044Eu, 0x044Fu, 0x0023u, 0x2116u, 0x002Eu, 0x002Cu,
            0x003Bu, 0x003Au, 0x0021u, 0x003Fu, 0x0028u, 0x0029u,
            0x007Bu, 0x007Du, 0x00ABu, 0x00BBu, 0x2013u, 0x2014u,
            0x00D7u, 0x00B7u, 0x00B0u
        };

        constexpr int kGlyphIndexQuestion = 135;  // '?'
        constexpr GlyphMetric kSpaceMetric = { 0, 0, 0, 0, kSpaceAdv };
        constexpr uint8_t kBlankCell[ kGlyphBytes ] = {};

        constexpr int question_index() noexcept
        {
            for ( int i = 0; i < kGlyphCount; ++i )
            {
                if ( kCodepoints[ i ] == 0x3Fu ) return i;
            }
            return -1;
        }
        static_assert( question_index() == 135, "charset order mismatch" );

        const GlyphMetric& metric_at( int idx ) noexcept
        {
            if ( idx == kGlyphSpace ) return kSpaceMetric;
            if ( idx < 0 || idx >= kGlyphCount ) return kGlyphMetrics[ kGlyphIndexQuestion ];
            return kGlyphMetrics[ idx ];
        }
    }

    const uint8_t* glyph_alpha( int idx ) noexcept
    {
        if ( idx == kGlyphSpace ) return kBlankCell;
        if ( idx < 0 || idx >= kGlyphCount ) idx = kGlyphIndexQuestion;
        return kAtlas + idx * kGlyphBytes;
    }

    int glyph_index( uint32_t cp ) noexcept
    {
        if ( cp == 0x20u ) return kGlyphSpace;
        for ( int i = 0; i < kGlyphCount; ++i )
        {
            if ( kCodepoints[ i ] == cp ) return i;
        }
        return kGlyphIndexQuestion;
    }

    uint32_t next_utf8( const char* str, int len, int& pos ) noexcept
    {
        if ( pos >= len ) return 0;
        const auto c0 = static_cast<uint8_t>( str[ pos ] );
        if ( c0 < 0x80u ) { ++pos; return c0; }

        int need = 0;
        uint32_t cp = 0;
        if ( ( c0 & 0xE0u ) == 0xC0u ) { need = 1; cp = c0 & 0x1Fu; }
        else if ( ( c0 & 0xF0u ) == 0xE0u ) { need = 2; cp = c0 & 0x0Fu; }
        else if ( ( c0 & 0xF8u ) == 0xF0u ) { need = 3; cp = c0 & 0x07u; }

        if ( need == 0 ) { ++pos; return '?'; }
        if ( pos + need >= len ) { pos = len; return '?'; }
        for ( int i = 1; i <= need; ++i )
        {
            const auto cc = static_cast<uint8_t>( str[ pos + i ] );
            if ( ( cc & 0xC0u ) != 0x80u ) { ++pos; return '?'; }
            cp = ( cp << 6 ) | ( cc & 0x3Fu );
        }
        pos += need + 1;

        constexpr uint32_t k_min_cp[ 4 ] = { 0, 0x80u, 0x800u, 0x10000u };
        if ( cp < k_min_cp[ need ] || cp > 0x10FFFFu || ( cp >= 0xD800u && cp <= 0xDFFFu ) ) return '?';
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
            if ( cp == 0 ) break;
            ++count;
        }
        
        return count;
    }

    void build_atlas( std::array<uint8_t, kAtlasBytes>& atlas ) noexcept
    {
        std::memcpy( atlas.data(), kAtlas, kAtlasBytes );
    }

    void glyph_uv( int idx, float& u0, float& v0, float& u1, float& v1 ) noexcept
    {
        const GlyphMetric& m = metric_at( idx );
        if ( m.w == 0 || m.h == 0 )
        {
            u0 = 0.0f; v0 = 0.0f; u1 = 0.0f; v1 = 0.0f;
            return;
        }
        const int row = ( idx == kGlyphSpace ? 0 : idx ) * kGlyphH;
        u0 = static_cast<float>( m.dx ) / static_cast<float>( kAtlasW );
        v0 = static_cast<float>( row + m.dy ) / static_cast<float>( kAtlasH );
        u1 = static_cast<float>( m.dx + m.w ) / static_cast<float>( kAtlasW );
        v1 = static_cast<float>( row + m.dy + m.h ) / static_cast<float>( kAtlasH );
    }

    void solid_uv( float& u, float& v ) noexcept
    {
        u = ( static_cast<float>( kSolidX ) + 0.5f ) / static_cast<float>( kAtlasW );
        v = ( static_cast<float>( kSolidY ) + 0.5f ) / static_cast<float>( kAtlasH );
    }

    int format_person( char* dst, int dst_cap, int id, bool lang_ru ) noexcept
    {
        if ( dst == nullptr || dst_cap <= 0 ) return 0;
        int n = 0;
        const auto put = [ &dst, &n, dst_cap ]( char c )
        {
            if ( n + 1 < dst_cap ) dst[ n++ ] = c;
        };
        if ( lang_ru )
        {
            for ( const char* p = "Человек #"; *p != 0; ++p ) put( *p );
        }
        else
        {
            for ( const char* p = "Person #"; *p != 0; ++p ) put( *p );
        }
        if ( id < 0 ) id = 0;
        char digits[ 10 ];
        int d = 0;
        do
        {
            digits[ d++ ] = static_cast<char>( '0' + id % 10 );
            id /= 10;
        }
        while ( id != 0 && d < 10 );
        while ( d > 0 ) put( digits[ --d ] );
        dst[ n ] = 0;
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
            if ( cp == 0 ) break;
            w += metric_at( glyph_index( cp ) ).adv;
        }
        
        return w;
    }

    const GlyphMetric& glyph_metrics( int idx ) noexcept
    {
        return metric_at( idx );
    }

}  // namespace glyph_font
