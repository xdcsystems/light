#include "check.hpp"
#include "core/encoder_config.hpp"
#include "core/geometry.hpp"
#include "suites.hpp"

namespace
{
    bool same( const LbGeom& g, int left, int w, int top, int h )
    {
        return g.left == left && g.w == w && g.top == top && g.h == h;
    }

    bool size_is( const StreamLayout& l, int w, int h, int tw, int th, int nsrc, int vstride )
    {
        return l.w == w && l.h == h && l.tile_w == tw && l.tile_h == th && l.nsrc == nsrc && l.vstride == vstride;
    }

    // The canonical encoder band of the Pipeline ctor: 4:3 of the model input.
    const LbGeom kBand43 { 0, 640, 80, 480 };

    void layout_single( TestRun& t )
    {
        // The band at its native size.
        const StreamLayout l = make_stream_layout( 640, 640, kBand43, 1 );
        CHECK( t, size_is( l, 640, 480, 640, 480, 1, 512 ) );
        CHECK( t, same( l.band, 0, 640, 80, 480 ) );
        // nsrc <= 0 is single view too.
        CHECK( t, size_is( make_stream_layout( 640, 640, kBand43, 0 ), 640, 480, 640, 480, 1, 512 ) );
        // Portrait band: the width is 16-aligned, the height is rounded up to even (>= 2),
        // the vertical stride is 64-aligned, and l.band keeps the original odd height.
        const StreamLayout p = make_stream_layout( 640, 640, LbGeom { 140, 360, 0, 639 }, 1 );
        CHECK( t, size_is( p, 368, 640, 368, 640, 1, 640 ) );
        CHECK( t, p.band.h == 639 );
        // Edge cases: odd 1 -> 2, zero -> 2, already-even 480 -> 480.
        CHECK( t, make_stream_layout( 640, 640, LbGeom { 0, 320, 0, 1 }, 1 ).h == 2 );
        CHECK( t, make_stream_layout( 640, 640, LbGeom { 0, 320, 0, 0 }, 1 ).h == 2 );
        CHECK( t, make_stream_layout( 640, 640, LbGeom { 0, 320, 0, 480 }, 1 ).h == 480 );
        // A band outside the tensor is clamped into it (the stream size is not).
        const StreamLayout c = make_stream_layout( 640, 640, LbGeom { -4, 700, -2, 650 }, 1 );
        CHECK( t, same( c.band, 0, 640, 0, 640 ) );
        CHECK( t, c.w == 704 && c.h == 650 );
    }

    void layout_multi( TestRun& t )
    {
        // 4:3 cells of the model input, 2x1 for two sources, 2x2 from three on.
        CHECK( t, size_is( make_stream_layout( 640, 640, kBand43, 2 ), 1280, 480, 640, 480, 2, 512 ) );
        CHECK( t, size_is( make_stream_layout( 640, 640, kBand43, 3 ), 1280, 960, 640, 480, 3, 960 ) );
        CHECK( t, size_is( make_stream_layout( 640, 640, kBand43, 4 ), 1280, 960, 640, 480, 4, 960 ) );
        // Beyond the 2x2 grid: clamped to 4.
        CHECK( t, size_is( make_stream_layout( 640, 640, kBand43, 5 ), 1280, 960, 640, 480, 4, 960 ) );
        // Non-aligned model input: both cell sides 16-aligned (456 -> 464), vstride 64-aligned (928 -> 960).
        CHECK( t, size_is( make_stream_layout( 608, 608, kBand43, 3 ), 1216, 928, 608, 464, 3, 960 ) );
    }

    void cells( TestRun& t )
    {
        const StreamLayout l = make_stream_layout( 640, 640, kBand43, 3 );
        CHECK( t, same( cell_rect( l, 0 ), 0, 640, 0, 480 ) );
        CHECK( t, same( cell_rect( l, 1 ), 640, 640, 0, 480 ) );
        CHECK( t, same( cell_rect( l, 2 ), 0, 640, 480, 480 ) );
        CHECK( t, same( cell_rect( l, 3 ), 640, 640, 480, 480 ) );
    }

    void placement_single( TestRun& t )
    {
        const StreamLayout l = make_stream_layout( 640, 640, kBand43, 1 );
        // The frame's band equals the layout band: copied 1:1 to the origin.
        TilePlacement p = place_tile( l, kBand43, 0 );
        CHECK( t, p.cell == 0 );
        CHECK( t, same( p.src, 0, 640, 80, 480 ) );
        CHECK( t, same( p.dst, 0, 640, 0, 480 ) );
        // No band with the frame: the layout band stands in.
        p = place_tile( l, LbGeom {}, 0 );
        CHECK( t, same( p.src, 0, 640, 80, 480 ) );
        CHECK( t, same( p.dst, 0, 640, 0, 480 ) );
        // src_idx is ignored in single view.
        CHECK( t, place_tile( l, kBand43, 2 ).cell == 0 );
        // A different band is letterboxed into the stream band; odd values are evened.
        const LbGeom odd { 141, 359, 3, 641 };
        p = place_tile( l, odd, 0 );
        CHECK( t, same( p.src, 140, 358, 2, 640 ) );
        const LbGeom g = band_geom( 359, 641, 640, 480 );
        CHECK( t, same( p.dst, g.left & ~1, g.w & ~1, g.top & ~1, g.h & ~1 ) );
        // Odd single-view layout rounds h to 640 while place_tile produces even dst.h <= p.h.
        const StreamLayout odd_l = make_stream_layout( 640, 640, LbGeom { 140, 360, 0, 639 }, 1 );
        const TilePlacement odd_tp = place_tile( odd_l, LbGeom { 140, 360, 0, 639 }, 0 );
        CHECK( t, ( odd_tp.dst.h & 1 ) == 0 && odd_tp.dst.h <= odd_l.h );
    }

    void placement_multi( TestRun& t )
    {
        const StreamLayout l = make_stream_layout( 640, 640, kBand43, 3 );
        // A 4:3 band fills its cell exactly.
        TilePlacement p = place_tile( l, kBand43, 1 );
        CHECK( t, p.cell == 1 );
        CHECK( t, same( p.dst, 640, 640, 0, 480 ) );
        // A portrait band is letterboxed into cell 2 (bottom left), offset by the cell origin.
        const LbGeom portrait { 140, 360, 0, 640 };
        p = place_tile( l, portrait, 2 );
        const LbGeom g = band_geom( 360, 640, 640, 480 );
        CHECK( t, p.cell == 2 );
        CHECK( t, same( p.dst, g.left & ~1, g.w & ~1, ( 480 + g.top ) & ~1, g.h & ~1 ) );
        // The tile scale the callout font compensates (#91 item 5): the phone
        // portrait band 360x640 lands as 270x480 (0.75), a 4:3 USB band 1:1.
        const TilePlacement usb = place_tile( l, kBand43, 0 );
        CHECK( t, usb.dst.h == usb.src.h && usb.dst.w == usb.src.w );                    // 1.0
        CHECK( t, p.src.h == 640 && p.dst.h == 480 && p.dst.w == 270 );                    // 0.75
        // Cell 3 of a 3-source layout does not exist: out-of-range indices fall back to cell 0.
        CHECK( t, place_tile( l, kBand43, 3 ).cell == 0 );
        CHECK( t, place_tile( l, kBand43, -1 ).cell == 0 );
    }

    void absent_mask( TestRun& t )
    {
        const bool m[2] = { false, true };
        CHECK( t, !tile_absent( m, 2, 0 ) );
        CHECK( t, tile_absent( m, 2, 1 ) );
        CHECK( t, tile_absent( m, 2, 2 ) );  // beyond the mask: absent
        CHECK( t, tile_absent( m, 1, 1 ) );
        CHECK( t, tile_absent( nullptr, 0, 0 ) );  // no mask: every tile absent
    }

    void rates( TestRun& t )
    {
        EncoderConfig c;
        c.layout = make_stream_layout( 640, 640, kBand43, 1 );
        c.fps = 30;
        CHECK( t, !c.vfr() && c.nominal_fps() == 30 && c.gop() == 30 );
        c.fps = 0;  // follow the source: nominal 25 for rate control, GOP 1 s of it
        CHECK( t, c.vfr() && c.nominal_fps() == 25 && c.gop() == 25 );
        c.layout = make_stream_layout( 640, 640, kBand43, 3 );
        CHECK( t, c.nominal_fps() == 25 && c.gop() == 25 );
        c.fps = 25;
        CHECK( t, c.nominal_fps() == 25 && c.gop() == 25 );
        // Bitrate: as configured, and 12000 kbps for an unset / invalid value (never 0 to MPP).
        c.bitrate_kbps = 8000;
        CHECK( t, c.kbps() == 8000 && c.bit_rate() == 8000000 );
        c.bitrate_kbps = 0;
        CHECK( t, c.kbps() == 12000 && c.bit_rate() == 12000000 );
        c.bitrate_kbps = -5;
        CHECK( t, c.kbps() == 12000 );
    }
}  // namespace

void test_encoder_config( TestRun& t )
{
    layout_single( t );
    layout_multi( t );
    cells( t );
    placement_single( t );
    placement_multi( t );
    absent_mask( t );
    rates( t );
}
