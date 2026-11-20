#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/encoder_config.hpp"
#include "core/glyph_font.hpp"
#include "core/idle_plate.hpp"
#include "suites.hpp"

namespace
{
    const uint8_t kGray = 128;

    struct Box
    {
        // One ink pixel at column c, row r: grows the box, counts it.
        void add( int c, int r )
        {
            ++n;
            x0 = c < x0 ? c : x0;
            x1 = c > x1 ? c : x1;
            y0 = r < y0 ? r : y0;
            y1 = r > y1 ? r : y1;
        }

    public:
        int x0 = 1 << 30;
        int y0 = 1 << 30;
        int x1 = -1;  // inclusive
        int y1 = -1;
        int n = 0;
    };

    // Bounding box of the pixels != gray inside [x, x + w) x [y, y + h).
    Box ink( const std::vector<uint8_t>& p, int stride, int x, int y, int w, int h )
    {
        Box b;
        for ( int r = y; r < y + h; ++r )
            for ( int c = x; c < x + w; ++c )
                if ( p[static_cast<size_t>( r ) * stride + c] != kGray )
                    b.add( c, r );
        return b;
    }

    // The glyph fill footprint of the plate in one tile: 1 where raster_text samples a
    // fill > 0 for the label at the plate's pen and scale, 0 elsewhere.
    std::vector<uint8_t> fill_mask( const IdlePlate& plate, const StreamLayout& l )
    {
        std::vector<uint8_t> m( static_cast<size_t>( l.tile_w ) * l.tile_h, 0 );
        const std::string text = plate.text();
        glyph_font::raster_text( text.data(), static_cast<int>( text.size() ), plate.pen_x( l ), plate.pen_y( l ), plate.scale(),
                                 l.tile_w, l.tile_h, [&m, &l]( int x, int y, const glyph_font::Texel2& s ) {
                                     if ( s.fill > 0.0f )
                                         m[static_cast<size_t>( y ) * l.tile_w + x] = 1;
                                 } );
        return m;
    }

    // The plate on a canvas of `layout`: one label per tile, all identical, every
    // pixel within [luma, gray] (antialiased dark text, never lighter than the
    // canvas), no halo (a pixel the fill does not reach stays exactly gray: no
    // outline), and in every tile the ink box of the label: ink_w x ink_h within
    // +-tol_w / +-1 px, centered within 1 px (twice the offset of its center from the
    // tile center <= 2); darkest luma 16.
    void plate_on( TestRun& t, const StreamLayout& l, int ink_w, int tol_w, int ink_h )
    {
        const IdlePlate plate {};
        std::vector<uint8_t> p( static_cast<size_t>( l.w ) * l.vstride, kGray );
        plate.draw( Plane8 { p.data(), l.w, l.vstride }, l );
        CHECK( t, plate.luma() == 16 );
        int darkest = 255;
        bool in_range = true;
        for ( const uint8_t v : p )
        {
            darkest = v < darkest ? v : darkest;
            in_range = in_range && v >= plate.luma() && v <= kGray;
        }
        CHECK_EQ( t, darkest, 16 );
        CHECK( t, in_range );
        const std::vector<uint8_t> mask = fill_mask( plate, l );
        Box first;
        int total = 0;
        for ( int s = 0; s < l.nsrc; ++s )
        {
            const LbGeom c = cell_rect( l, s );
            const Box b = ink( p, l.w, c.left, c.top, c.w, c.h );
            total += b.n;
            CHECK( t, b.n > 0 );
            CHECK( t, std::abs( ( b.x1 - b.x0 + 1 ) - ink_w ) <= tol_w );
            CHECK( t, std::abs( ( b.y1 - b.y0 + 1 ) - ink_h ) <= 1 );
            CHECK( t, std::abs( ( b.x0 + b.x1 + 1 ) - ( 2 * c.left + c.w ) ) <= 2 );
            CHECK( t, std::abs( ( b.y0 + b.y1 + 1 ) - ( 2 * c.top + c.h ) ) <= 2 );
            bool no_halo = true;
            for ( int y = 0; y < c.h; ++y )
                for ( int x = 0; x < c.w; ++x )
                    no_halo = no_halo && ( mask[static_cast<size_t>( y ) * l.tile_w + x] != 0 ||
                                           p[static_cast<size_t>( c.top + y ) * l.w + c.left + x] == kGray );
            CHECK( t, no_halo );
            if ( s == 0 )
                first = b;
            else  // the same label, translated by the cell origin
                CHECK( t, b.n == first.n && b.x0 - c.left == first.x0 && b.y0 - c.top == first.y0 );
        }
        CHECK( t, total == ink( p, l.w, 0, 0, l.w, l.vstride ).n );  // no ink outside the tiles
    }

    // NO SIGNAL in the label font (#108), at the size of the former stroke-font label
    // (156 x 21 px of ink): 21 px capitals, the width within 4 px.
    void plate( TestRun& t )
    {
        const LbGeom band { 0, 640, 80, 480 };
        for ( int n = 1; n <= 4; ++n )  // single view, 2x1, 2x2 with 3 and 4 tiles
            plate_on( t, make_stream_layout( 640, 640, band, n ), 156, 4, 21 );
        const StreamLayout l3 = make_stream_layout( 640, 640, band, 3 );
        const IdlePlate pl {};
        std::vector<uint8_t> p( static_cast<size_t>( l3.w ) * l3.vstride, kGray );
        pl.draw( Plane8 { p.data(), l3.w, l3.vstride }, l3 );
        const LbGeom c3 = cell_rect( l3, 3 );
        CHECK( t, ink( p, l3.w, c3.left, c3.top, c3.w, c3.h ).n == 0 );  // the 4th cell stays gray
        CHECK_NEAR( t, pl.scale(), 21.0f / 16.0f, 1e-5f );                // 21 px over the 16 px atlas capital
    }

    // A plane shorter and narrower than the layout (inside a larger buffer): the label is
    // clipped, nothing is written outside the plane; a null plane is a no-op.
    void plate_clipped( TestRun& t )
    {
        const LbGeom band { 0, 640, 80, 480 };
        const StreamLayout l = make_stream_layout( 640, 640, band, 1 );
        const int stride = 300;          // the clip width: the label's right part is cut
        const int h = l.tile_h / 2 + 4;  // the label's bottom rows are cut
        const int guard = 8;
        std::vector<uint8_t> p( static_cast<size_t>( stride ) * ( h + guard ), kGray );
        const IdlePlate plate {};
        plate.draw( Plane8 { p.data(), stride, h }, l );
        CHECK( t, ink( p, stride, 0, 0, stride, h ).n > 0 );
        CHECK( t, ink( p, stride, 0, h, stride, guard ).n == 0 );
        // The label starts at x ~ 241: a column past the clip width would wrap into the
        // next row, at x < 200.
        CHECK( t, ink( p, stride, 0, 0, 200, h + guard ).n == 0 );
        plate.draw( Plane8 { nullptr, stride, h }, l );

        // 2x1 on the same narrow plane: the second tile (x >= 640) lies wholly outside
        // it and is skipped; the first is drawn exactly as in single view.
        const StreamLayout l2 = make_stream_layout( 640, 640, band, 2 );
        std::vector<uint8_t> q( static_cast<size_t>( stride ) * ( h + guard ), kGray );
        plate.draw( Plane8 { q.data(), stride, h }, l2 );
        CHECK( t, ink( q, stride, 0, h, stride, guard ).n == 0 );
        CHECK( t, ink( q, stride, 0, 0, 200, h + guard ).n == 0 );
        CHECK( t, ink( q, stride, 0, 0, stride, h ).n == ink( p, stride, 0, 0, stride, h ).n );
    }
}  // namespace

void test_idle_plate( TestRun& t )
{
    plate( t );
    plate_clipped( t );
}
