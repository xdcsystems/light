// The CPU overlay raster (core/overlay_raster.hpp): box outlines and the outlined
// callout text on RGB888 tiles, clipped to the tile. With LIGHT_AI_DUMP_DIR set, the
// callout tiles are also written there as PPM (callout_ls<N>_<bg>.ppm: the
// --label-scale N label on a light / dark tile) for a visual before / after check;
// a normal run writes nothing.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/callout.hpp"
#include "core/glyph_font.hpp"
#include "core/gpu_box.hpp"
#include "core/overlay_raster.hpp"
#include "suites.hpp"

namespace
{
    constexpr uint8_t kGuard = 0xA5;
    constexpr int kGuardPx = 2;  // guard pixels left and right of every row, guard rows above and below

    // An RGB888 tile of w x h pixels inside a larger buffer with a kGuardPx frame on every
    // side: rows stride bytes apart. The tile is filled with gray bg, the frame with kGuard.
    struct Tile
    {
        Tile( int tw, int th, uint8_t bg ) : w( tw ), h( th ), stride( ( tw + 2 * kGuardPx ) * 3 )
        {
            buf.assign( static_cast<size_t>( stride ) * static_cast<size_t>( h + 2 * kGuardPx ), kGuard );
            for ( int y = 0; y < h; ++y )
                for ( int x = 0; x < w * 3; ++x )
                    buf[offset( 0, y ) + x] = bg;
        }

        // Byte offset of tile pixel (x, y) in buf (the frame: x or y in [-kGuardPx, 0) or past w / h).
        size_t offset( int x, int y ) const
        {
            return static_cast<size_t>( y + kGuardPx ) * stride + static_cast<size_t>( x + kGuardPx ) * 3;
        }

        uint8_t* data() { return buf.data() + offset( 0, 0 ); }
        const uint8_t* px( int x, int y ) const { return buf.data() + offset( x, y ); }

        // Nothing written outside the tile: every byte of the frame keeps kGuard.
        bool guard_intact() const
        {
            for ( int y = -kGuardPx; y < h + kGuardPx; ++y )
                for ( int x = -kGuardPx; x < w + kGuardPx; ++x )
                {
                    const bool in_tile = x >= 0 && x < w && y >= 0 && y < h;
                    const uint8_t* p = px( x, y );
                    if ( !in_tile && ( p[0] != kGuard || p[1] != kGuard || p[2] != kGuard ) )
                        return false;
                }
            return true;
        }

        void dump( const std::string& path ) const
        {
            std::ofstream f( path, std::ios::binary );
            f << "P6\n" << w << ' ' << h << "\n255\n";
            for ( int y = 0; y < h; ++y )
                f.write( reinterpret_cast<const char*>( px( 0, y ) ), static_cast<std::streamsize>( w ) * 3 );
        }

    public:
        int w;
        int h;
        int stride;
        std::vector<uint8_t> buf;
    };

    // Gray statistics of the tile pixels that differ from bg (the text and its outline).
    struct Ink
    {
        int n = 0;
        int min_gray = 255;  // darkest neutral pixel (r == g == b)
        int max_gray = 0;    // brightest neutral pixel
        bool neutral = true;  // every changed pixel outside the connector is gray (r == g == b)
    };

    // The pixels of rect [x0, x1) x [y0, y1) of the tile that are not bg.
    Ink ink_in( const Tile& t, int x0, int y0, int x1, int y1, uint8_t bg )
    {
        Ink k;
        for ( int y = y0; y < y1; ++y )
            for ( int x = x0; x < x1; ++x )
            {
                const uint8_t* p = t.px( x, y );
                if ( p[0] == bg && p[1] == bg && p[2] == bg )
                    continue;
                ++k.n;
                k.neutral = k.neutral && p[0] == p[1] && p[1] == p[2];
                k.min_gray = p[0] < k.min_gray ? p[0] : k.min_gray;
                k.max_gray = p[0] > k.max_gray ? p[0] : k.max_gray;
            }
        return k;
    }

    // "Person #7" at --label-scale ls on a 1:1 tile (CalloutItem::scale 0.5 * ls), its pen
    // at (pad, pad), a 1-unit vertical connector at the tile's left edge in an accent color.
    CalloutItem person_callout( int ls, int pad )
    {
        CalloutItem c {};
        c.scale = callout_text_scale( ls, 1.0f );
        c.outline = 2.0f * c.scale;
        c.text_bytes = glyph_font::format_person( c.text, glyph_font::kLabelCap, 7, false );
        c.text_x = pad;
        c.text_y = pad;
        c.r = 255;
        c.g = 64;
        c.b = 0;
        c.segs[0] = { 1, 1, 1 + ls, pad + 4 };
        c.n_segs = 1;
        return c;
    }

    void callouts( TestRun& t )
    {
        const char* dump_dir = std::getenv( "LIGHT_AI_DUMP_DIR" );
        for ( int ls = 1; ls <= 3; ++ls )
        {
            for ( const uint8_t bg : { uint8_t { 200 }, uint8_t { 30 } } )
            {
                const int pad = 6 * ls;
                const CalloutItem c = person_callout( ls, pad );
                const int tw = static_cast<int>( std::ceil( static_cast<float>( glyph_font::text_width( "Person #7" ) ) * c.scale ) );
                const int th = static_cast<int>( std::ceil( static_cast<float>( glyph_font::kLineH ) * c.scale ) );
                Tile tile( tw + 2 * pad, th + 2 * pad, bg );
                CalloutList list {};
                list.push( c );
                draw_callouts_cpu( tile.data(), tile.w, tile.h, tile.stride, list );
                CHECK( t, tile.guard_intact() );

                // The connector: exactly its color, opaque.
                const uint8_t* seg = tile.px( 1, 2 );
                CHECK( t, seg[0] == 255 && seg[1] == 64 && seg[2] == 0 );

                // The text with its outline: neutral gray, white at the core, dark around it.
                const Ink k = ink_in( tile, 1 + ls, 0, tile.w, tile.h, bg );
                CHECK( t, k.n > 0 );
                CHECK( t, k.neutral );
                CHECK( t, k.max_gray >= 240 );
                CHECK( t, k.min_gray <= ( bg > 100 ? 48 : 20 ) );
                // Nothing far from the text: the outline stays within about 2 * scale of the ink,
                // and the ink starts at the pen (the line top) or after it.
                // On the right and at the bottom the margin is the pad past the advance box / line box.
                const int far = pad - static_cast<int>( std::ceil( 4.0f * c.scale ) );
                CHECK( t, ink_in( tile, 1 + ls, 0, far, tile.h, bg ).n == 0 );
                CHECK( t, ink_in( tile, 1 + ls, 0, tile.w, far, bg ).n == 0 );
                CHECK( t, ink_in( tile, tile.w - far, 0, tile.w, tile.h, bg ).n == 0 );
                CHECK( t, ink_in( tile, 1 + ls, tile.h - far, tile.w, tile.h, bg ).n == 0 );
                if ( dump_dir != nullptr )
                    tile.dump( std::string( dump_dir ) + "/callout_ls" + std::to_string( ls ) + ( bg > 100 ? "_light" : "_dark" )
                               + ".ppm" );
            }
        }

        // A callout running off every edge of a small tile: clipped, nothing outside.
        Tile small( 24, 10, 128 );
        CalloutItem c = person_callout( 2, 0 );
        c.text_x = -20;
        c.text_y = -6;
        c.segs[0] = { -5, -5, 40, 3 };
        CalloutList list {};
        list.push( c );
        draw_callouts_cpu( small.data(), small.w, small.h, small.stride, list );
        CHECK( t, small.guard_intact() );
        CHECK( t, ink_in( small, 0, 0, small.w, small.h, 128 ).n > 0 );
    }

    void boxes( TestRun& t )
    {
        // An opaque box: its 1 px perimeter in the box color, the inside untouched.
        Tile tile( 40, 30, 100 );
        const GpuBox opaque { 4.0f, 5.0f, 14.0f, 20.0f, 250, 10, 20, 255 };
        draw_boxes_cpu( tile.data(), tile.w, tile.h, tile.stride, &opaque, 1 );
        const uint8_t* corner = tile.px( 4, 5 );
        const uint8_t* edge = tile.px( 13, 12 );
        const uint8_t* inside = tile.px( 8, 12 );
        CHECK( t, corner[0] == 250 && corner[1] == 10 && corner[2] == 20 );
        CHECK( t, edge[0] == 250 && edge[1] == 10 && edge[2] == 20 );
        CHECK( t, inside[0] == 100 && inside[1] == 100 && inside[2] == 100 );
        CHECK( t, tile.px( 14, 12 )[0] == 100 && tile.px( 8, 20 )[0] == 100 );  // x1, y1 exclusive

        // A translucent box: every perimeter pixel blended once (corners match the edges).
        const GpuBox half { 20.0f, 5.0f, 30.0f, 15.0f, 255, 255, 255, 128 };
        draw_boxes_cpu( tile.data(), tile.w, tile.h, tile.stride, &half, 1 );
        const int want = ( 255 * 128 + 100 * 127 + 127 ) / 255;
        CHECK( t, tile.px( 20, 5 )[0] == want && tile.px( 25, 5 )[0] == want && tile.px( 29, 14 )[0] == want );

        // Off-screen, degenerate and edge-crossing boxes: clipped, nothing outside the tile.
        const GpuBox rest[] = {
            { -50.0f, -50.0f, -10.0f, -10.0f, 0, 0, 0, 255 },  // off-screen
            { 5.0f, 25.0f, 5.5f, 28.0f, 0, 0, 0, 255 },        // < 1 px wide
            { 30.0f, 20.0f, 90.0f, 90.0f, 0, 0, 0, 255 },      // past the right and bottom edges
        };
        draw_boxes_cpu( tile.data(), tile.w, tile.h, tile.stride, rest, 3 );
        CHECK( t, tile.guard_intact() );
        CHECK( t, tile.px( 5, 25 )[0] == 100 );
        CHECK( t, tile.px( 39, 29 )[0] == 0 && tile.px( 35, 20 )[0] == 0 );
    }
}  // namespace

void test_overlay_raster( TestRun& t )
{
    callouts( t );
    boxes( t );
}
