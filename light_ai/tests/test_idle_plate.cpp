#include <cstddef>
#include <cstdint>
#include <vector>

#include "check.hpp"
#include "core/bitmap_font.hpp"
#include "core/encoder_config.hpp"
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

    bool only( const std::vector<uint8_t>& p, uint8_t a, uint8_t b )
    {
        for ( const uint8_t v : p )
            if ( v != a && v != b )
                return false;
        return true;
    }

    void font( TestRun& t )
    {
        BitmapFont f;
        CHECK( t, f.width( nullptr, 3 ) == 0 && f.height( nullptr, 3 ) == 0 );
        CHECK( t, f.width( "", 3 ) == 0 );
        const int w1 = f.width( "NO SIGNAL", 1 );
        CHECK( t, w1 > 0 && f.width( "NO SIGNAL", 3 ) == 3 * w1 );
        CHECK( t, f.height( "NO SIGNAL", 3 ) == 3 * f.height( "NO SIGNAL", 1 ) );
        CHECK( t, f.width( "NO", 1 ) < w1 );

        // Ink stays inside [x, x + width) x [y, y + height); only val is written.
        const int stride = 200;
        const int h = 60;
        std::vector<uint8_t> p( static_cast<size_t>( stride ) * h, kGray );
        f.stamp( Plane8 { p.data(), stride, h }, 10, 20, "NO SIGNAL", 2, 16 );
        const Box b = ink( p, stride, 0, 0, stride, h );
        CHECK( t, b.n > 0 );
        CHECK( t, b.x0 >= 10 && b.x1 < 10 + f.width( "NO SIGNAL", 2 ) );
        CHECK( t, b.y0 >= 20 && b.y1 < 20 + f.height( "NO SIGNAL", 2 ) );
        CHECK( t, only( p, kGray, 16 ) );

        // A no-op on bad arguments.
        std::vector<uint8_t> q( static_cast<size_t>( stride ) * h, kGray );
        f.stamp( Plane8 { q.data(), stride, h }, 10, 20, nullptr, 2, 16 );
        f.stamp( Plane8 { q.data(), stride, h }, 10, 20, "NO", 0, 16 );
        f.stamp( Plane8 { nullptr, stride, h }, 10, 20, "NO", 2, 16 );
        CHECK( t, ink( q, stride, 0, 0, stride, h ).n == 0 );
        // Long string within aligned kTextCap=32 / kVertexBytes=24576 renders completely without vertex truncation.
        std::vector<uint8_t> lg21( static_cast<size_t>( 400 ) * 20, kGray );
        std::vector<uint8_t> lg31( static_cast<size_t>( 400 ) * 20, kGray );
        f.stamp( Plane8 { lg21.data(), 400, 20 }, 0, 2, "WWWWWWWWWWWWWWWWWWWWW", 1, 16 );
        f.stamp( Plane8 { lg31.data(), 400, 20 }, 0, 2, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", 1, 16 );
        CHECK( t, ink( lg31, 400, 0, 0, 400, 20 ).n > ink( lg21, 400, 0, 0, 400, 20 ).n );

        // Non-ASCII UTF-8, DEL (0x7F), and control bytes are sanitized to blanks (no OOB UB).
        std::vector<uint8_t> u( static_cast<size_t>( stride ) * h, kGray );
        CHECK( t, f.width( "\xd1\x87\xd0\xb5\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb5\xd0\xba 1", 1 ) > 0 );
        CHECK( t, f.height( "A\x7f\x01\x1f\nB", 1 ) > 0 );
        f.stamp( Plane8 { u.data(), stride, h }, 10, 20, "\xd1\x87\xd0\xb5\xd0\xbb\x7f\x01 1", 1, 16 );
        CHECK( t, only( u, kGray, 16 ) );
        CHECK( t, ink( u, stride, 0, 0, stride, h ).n > 0 );  // '1' still renders
    }

    void clipping( TestRun& t )
    {
        // A label running off the right and bottom edges and off the top left:
        // nothing outside the plane, no wrap into the next row. The plane sits
        // inside a larger buffer: the guard rows after it stay untouched.
        BitmapFont f;
        const int stride = 40;
        const int h = 12;
        const int guard = 4;
        std::vector<uint8_t> p( static_cast<size_t>( stride ) * ( h + guard ), kGray );
        const Plane8 pl { p.data(), stride, h };
        f.stamp( pl, 30, 6, "WWWW", 2, 16 );
        CHECK( t, ink( p, stride, 0, 0, 30, h ).n == 0 );   // columns left of x: no wrap
        CHECK( t, ink( p, stride, 0, h, stride, guard ).n == 0 );  // nothing below the plane
        CHECK( t, ink( p, stride, 30, 6, 10, 6 ).n > 0 );   // the visible part is drawn
        std::vector<uint8_t> q( static_cast<size_t>( stride ) * h, kGray );
        f.stamp( Plane8 { q.data(), stride, h }, -6, -6, "WWWW", 2, 16 );
        CHECK( t, ink( q, stride, 0, 0, stride, h ).n > 0 );
        CHECK( t, only( q, kGray, 16 ) );
    }

    // FNV-1a over the whole plane: pins every byte stamp() writes.
    uint64_t fnv1a( const std::vector<uint8_t>& p )
    {
        uint64_t h = 1469598103934665603ULL;
        for ( const uint8_t v : p )
        {
            h ^= v;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // The exact output of stamp() on a 200x60 gray plane, pinned before the
    // stage-6e split (#39): three scales, and labels clipped at each edge.
    void pinned( TestRun& t )
    {
        struct Case
        {
            int x;
            int y;
            const char* s;
            int scale;
            uint64_t hash;
            int n;  // pixels set to val
        };
        const Case cases[] = {
            { 10, 20, "NO SIGNAL", 1, 0x2477b1c55d14a243ULL, 110 },
            { 10, 20, "NO SIGNAL", 2, 0x576cc14b5b45be03ULL, 440 },
            { 4, 2, "NO SIGNAL", 3, 0x2d9603c6bcac54e3ULL, 990 },
            { -6, -6, "WWWW", 2, 0x5c34a4bb255d8003ULL, 192 },       // top left
            { 180, 50, "WWWW", 2, 0x94f4e32596918703ULL, 96 },       // right and bottom
            { -30, 30, "Ag-0:% s1", 2, 0x07f273305f8e6b83ULL, 204 },  // left
            { 150, -9, "LIGHT 42", 3, 0x6e28dfa79662ecd3ULL, 207 },   // top and right
        };
        const int stride = 200;
        const int h = 60;
        for ( const Case& c : cases )
        {
            BitmapFont f;
            std::vector<uint8_t> p( static_cast<size_t>( stride ) * h, kGray );
            f.stamp( Plane8 { p.data(), stride, h }, c.x, c.y, c.s, c.scale, 16 );
            CHECK( t, fnv1a( p ) == c.hash );
            CHECK( t, ink( p, stride, 0, 0, stride, h ).n == c.n );
        }
    }

    // The plate on a canvas of `layout`: one label per tile, all identical,
    // each centered by text_x / text_y, nothing outside the label boxes.
    void plate_on( TestRun& t, const StreamLayout& l )
    {
        IdlePlate plate;
        std::vector<uint8_t> p( static_cast<size_t>( l.w ) * l.vstride, kGray );
        plate.draw( Plane8 { p.data(), l.w, l.vstride }, l );
        CHECK( t, only( p, kGray, plate.luma() ) );
        CHECK( t, plate.luma() == 16 );
        const int tx = plate.text_x( l );
        const int ty = plate.text_y( l );
        CHECK( t, tx == ( l.tile_w - plate.text_w() ) / 2 && ty == ( l.tile_h - plate.text_h() ) / 2 );
        Box first;
        int total = 0;
        for ( int s = 0; s < l.nsrc; ++s )
        {
            const LbGeom c = cell_rect( l, s );
            const Box b = ink( p, l.w, c.left, c.top, c.w, c.h );
            total += b.n;
            CHECK( t, b.n > 0 );
            CHECK( t, b.x0 >= c.left + tx && b.x1 < c.left + tx + plate.text_w() );
            CHECK( t, b.y0 >= c.top + ty && b.y1 < c.top + ty + plate.text_h() );
            if ( s == 0 )
                first = b;
            else  // the same label, translated by the cell origin
                CHECK( t, b.n == first.n && b.x0 - c.left == first.x0 && b.y0 - c.top == first.y0 );
        }
        CHECK( t, total == ink( p, l.w, 0, 0, l.w, l.vstride ).n );  // no ink outside the tiles
    }

    // The label as the viewer sees it, in every tile of `l`: its ink box, its darkest luma,
    // and twice the offset of the ink box center from the tile center (x, y).
    void plate_ink_on( TestRun& t, const StreamLayout& l, int ink_w, int ink_h, int off2_x, int off2_y )
    {
        IdlePlate plate;
        std::vector<uint8_t> p( static_cast<size_t>( l.w ) * l.vstride, kGray );
        plate.draw( Plane8 { p.data(), l.w, l.vstride }, l );
        int darkest = 255;
        for ( const uint8_t v : p )
            darkest = v < darkest ? v : darkest;
        CHECK_EQ( t, darkest, 16 );
        for ( int s = 0; s < l.nsrc; ++s )
        {
            const LbGeom c = cell_rect( l, s );
            const Box b = ink( p, l.w, c.left, c.top, c.w, c.h );
            CHECK_EQ( t, b.x1 - b.x0 + 1, ink_w );
            CHECK_EQ( t, b.y1 - b.y0 + 1, ink_h );
            CHECK_EQ( t, ( b.x0 + b.x1 + 1 ) - ( 2 * c.left + c.w ), off2_x );
            CHECK_EQ( t, ( b.y0 + b.y1 + 1 ) - ( 2 * c.top + c.h ), off2_y );
        }
    }

    // The stb_easy_font label before #108, pinned: 156 x 21 px of ink at luma 16, 3 px left
    // and 7.5 px above the tile center (the stb cell is 162 x 36, its ink sits top left).
    void plate_ink( TestRun& t )
    {
        const LbGeom band { 0, 640, 80, 480 };
        plate_ink_on( t, make_stream_layout( 640, 640, band, 1 ), 156, 21, -6, -15 );
        plate_ink_on( t, make_stream_layout( 640, 640, band, 4 ), 156, 21, -6, -15 );
    }

    void plate( TestRun& t )
    {
        const LbGeom band { 0, 640, 80, 480 };
        plate_on( t, make_stream_layout( 640, 640, band, 1 ) );  // single view: once
        plate_on( t, make_stream_layout( 640, 640, band, 2 ) );  // 2x1
        plate_on( t, make_stream_layout( 640, 640, band, 3 ) );  // 2x2, three tiles: the 4th cell stays gray
        const StreamLayout l3 = make_stream_layout( 640, 640, band, 3 );
        IdlePlate pl;
        std::vector<uint8_t> p( static_cast<size_t>( l3.w ) * l3.vstride, kGray );
        pl.draw( Plane8 { p.data(), l3.w, l3.vstride }, l3 );
        const LbGeom c3 = cell_rect( l3, 3 );
        CHECK( t, ink( p, l3.w, c3.left, c3.top, c3.w, c3.h ).n == 0 );
    }
}  // namespace

void test_idle_plate( TestRun& t )
{
    font( t );
    clipping( t );
    pinned( t );
    plate( t );
    plate_ink( t );
}
