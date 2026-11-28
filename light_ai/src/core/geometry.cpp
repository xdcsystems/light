#include <algorithm>
#include <cstdint>

#include "core/geometry.hpp"

LbGeom band_geom( int w_src, int h_src, int w_dst, int h_dst )
{
    // Single owner of the letterbox formula: resize() and the Pipeline
    // ctor both call this, so the RGA crop, the encoder crop and the
    // postprocess scale always agree. Orientation-aware uniform fit
    // (integer truncation): the fit dimension is the tighter one —
    // landscape letterboxes vertically, portrait horizontally.
    LbGeom g;
    if ( w_src <= 0 || h_src <= 0 || w_dst <= 0 || h_dst <= 0 )
        return g;
    // 64-bit: the products of two sizes overflow int. Each quotient is capped at the destination size.
    const int64_t ws = w_src;
    const int64_t hs = h_src;
    const int64_t wd = w_dst;
    const int64_t hd = h_dst;
    const bool fit_by_h = ( hs * wd > ws * hd );  // portrait-ish
    if ( fit_by_h )
    {
        g.w = static_cast<int>( std::min( ws * hd / hs, wd ) );
        g.h = h_dst;
        g.left = ( w_dst - g.w ) / 2;
        g.top = 0;
    }
    else
    {
        g.w = w_dst;
        g.h = static_cast<int>( std::min( hs * wd / ws, hd ) );
        g.left = 0;
        g.top = ( h_dst - g.h ) / 2;
    }
    return g;
}
