#include <climits>

#include "check.hpp"
#include "core/geometry.hpp"
#include "suites.hpp"

namespace
{
    bool same( const LbGeom& g, int left, int w, int top, int h )
    {
        return g.left == left && g.w == w && g.top == top && g.h == h;
    }
}  // namespace

void test_geometry( TestRun& t )
{
    // Landscape sources letterbox vertically (the README examples).
    CHECK( t, same( band_geom( 1280, 720, 224, 224 ), 0, 224, 49, 126 ) );
    CHECK( t, same( band_geom( 1280, 720, 640, 640 ), 0, 640, 140, 360 ) );
    CHECK( t, same( band_geom( 1280, 960, 640, 640 ), 0, 640, 80, 480 ) );
    CHECK( t, same( band_geom( 1920, 1080, 640, 640 ), 0, 640, 140, 360 ) );
    // Portrait sources letterbox horizontally.
    CHECK( t, same( band_geom( 720, 1280, 640, 640 ), 140, 360, 0, 640 ) );
    // Same aspect: the band is the whole tensor.
    CHECK( t, same( band_geom( 640, 640, 640, 640 ), 0, 640, 0, 640 ) );
    // The canonical encoder band of the Pipeline ctor: 4:3 of the model input.
    CHECK( t, same( band_geom( 640, 480, 640, 640 ), 0, 640, 80, 480 ) );
    // Sizes whose cross products overflow int: both orientations and the int limit.
    CHECK( t, same( band_geom( 100000, 50000, 80000, 80000 ), 0, 80000, 20000, 40000 ) );
    CHECK( t, same( band_geom( 50000, 100000, 80000, 80000 ), 20000, 40000, 0, 80000 ) );
    CHECK( t, same( band_geom( INT_MAX, INT_MAX, INT_MAX, INT_MAX ), 0, INT_MAX, 0, INT_MAX ) );
    CHECK( t, same( band_geom( INT_MAX, 1, 640, 640 ), 0, 640, 320, 0 ) );
    CHECK( t, same( band_geom( 1, INT_MAX, 640, 640 ), 320, 0, 0, 640 ) );
}
