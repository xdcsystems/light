// core/rga_limits.hpp: the RK3588 RGA limits shared by RgaResizer,
// SourceChannel, Pipeline and RknnEngine (M-9, M-11).

#include "core/rga_limits.hpp"
#include "check.hpp"
#include "suites.hpp"

void test_rga_limits( TestRun& t )
{
    using namespace rga_limits;

    // RGA3 minimum side: 68 px (hardware probe: 64x64 rejected).
    CHECK( t, rga3_size_ok( 68, 68 ) );
    CHECK( t, !rga3_size_ok( 67, 640 ) );
    CHECK( t, !rga3_size_ok( 640, 64 ) );

    // One pass: 1/8x .. 8x per axis, both directions; zero sides never pass.
    CHECK( t, ratio_ok( 5120, 640 ) );   // exactly 8x down
    CHECK( t, !ratio_ok( 5121, 640 ) );
    CHECK( t, ratio_ok( 80, 640 ) );     // exactly 8x up
    CHECK( t, !ratio_ok( 79, 640 ) );
    CHECK( t, !ratio_ok( 0, 640 ) && !ratio_ok( 640, 0 ) );

    // Two-pass decision: over 8x of the dst on an axis; a quarter turn swaps the source axes.
    CHECK( t, !needs_two_pass( 1280, 960, 640, 640, false ) );
    CHECK( t, needs_two_pass( 5130, 960, 640, 640, false ) );
    CHECK( t, !needs_two_pass( 5120, 400, 640, 5200, false ) );  // exactly 8x: one pass
    CHECK( t, needs_two_pass( 400, 5130, 640, 5200, true ) );    // tall source, turned: its height meets dst width
    CHECK( t, !needs_two_pass( 400, 5130, 640, 5200, false ) );  // unturned it fits the tall dst

    // Aux sides: half, rounded up to even (an odd half would break NV12).
    CHECK( t, aux_side( 1280 ) == 640 );
    CHECK( t, aux_side( 1282 ) == 642 );   // 641 -> 642
    CHECK( t, aux_side( 1279 ) == 640 );   // 639 -> 640
    CHECK( t, aux_side( 2 ) == 2 );
    CHECK( t, aux_side( 6000 ) % 2 == 0 && aux_side( 6002 ) % 2 == 0 );
    // Aux bytes: 16-aligned strides of the aux sides, x2 over NV12 3/2.
    CHECK( t, aux_bytes( 6000, 4000 ) == static_cast<size_t>( 3008 ) * 2000 * 2 );
    CHECK( t, aux_bytes( 1282, 962 ) == static_cast<size_t>( 656 ) * 496 * 2 );
    CHECK( t, aux_bytes( 1282, 962 ) >= static_cast<size_t>( align16( aux_side( 1282 ) ) ) * align16( aux_side( 962 ) ) * 3 / 2 );

    // Even crop of YUV rects.
    CHECK( t, even_down( 721 ) == 720 && even_down( 720 ) == 720 && even_down( 1 ) == 0 );

    // RGB888 tensor rows: whole pixels, >= width, 16-aligned in pixels.
    CHECK( t, rgb888_stride_ok( 640 * 3, 640 ) );
    CHECK( t, rgb888_stride_ok( 128 * 3, 128 ) );
    CHECK( t, rgb888_stride_ok( 656 * 3, 650 ) );    // padded row
    CHECK( t, !rgb888_stride_ok( 650 * 3, 650 ) );   // tight but not 16-aligned
    CHECK( t, !rgb888_stride_ok( 640 * 3 + 1, 640 ) );  // not whole pixels
    CHECK( t, !rgb888_stride_ok( 624 * 3, 640 ) );   // shorter than the width
    CHECK( t, !rgb888_stride_ok( 640 * 3, 0 ) );
    CHECK( t, rgb888_stride_px( 656 * 3, 650 ) == 656 );
    CHECK( t, rgb888_stride_px( 0, 640 ) == 640 );   // unknown = tight
}
