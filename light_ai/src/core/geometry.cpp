#include "core/geometry.hpp"

LbGeom band_geom( int w_src, int h_src, int w_dst, int h_dst )
{
    // Single owner of the letterbox formula: resize() and the Pipeline
    // ctor both call this, so the RGA crop, the encoder crop and the
    // postprocess scale always agree. Orientation-aware uniform fit
    // (integer truncation): the fit dimension is the tighter one —
    // landscape letterboxes vertically, portrait horizontally.
    LbGeom g;
    const bool fit_by_h = ( h_src * w_dst > w_src * h_dst );  // portrait-ish
    if ( fit_by_h )
    {
        g.w = w_src * h_dst / h_src;
        if ( g.w > w_dst )
            g.w = w_dst;
        g.h = h_dst;
        g.left = ( w_dst - g.w ) / 2;
        g.top = 0;
    }
    else
    {
        g.w = w_dst;
        g.h = h_src * w_dst / w_src;
        if ( g.h > h_dst )
            g.h = h_dst;
        g.left = 0;
        g.top = ( h_dst - g.h ) / 2;
    }
    return g;
}
