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
}
