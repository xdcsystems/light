#include <algorithm>

#include "core/encoder_config.hpp"

namespace
{
    int align16( int v )
    {
        return ( v + 15 ) & ~15;
    }
}  // namespace

StreamLayout make_stream_layout( int model_w, int model_h, const LbGeom& band, int nsrc )
{
    StreamLayout l;
    l.nsrc = nsrc > 1 ? std::min( nsrc, static_cast<int>( StreamLayout::kMaxTiles ) ) : 1;
    // Single view: the band at its native size (the height as given, the
    // width 16-aligned); multi-view: a 4:3 cell of the model input.
    l.tile_w = l.nsrc > 1 ? align16( model_w ) : align16( band.w );
    l.tile_h = l.nsrc > 1 ? align16( model_h * 3 / 4 ) : band.h;
    l.w = l.nsrc > 1 ? l.tile_w * 2 : l.tile_w;
    l.h = l.nsrc > 2 ? l.tile_h * 2 : l.tile_h;
    l.vstride = align16( l.h );
    l.band.left = std::max( band.left, 0 );
    l.band.w = std::min( band.w, model_w );
    l.band.top = std::max( band.top, 0 );
    l.band.h = std::min( band.h, model_h );
    return l;
}

LbGeom cell_rect( const StreamLayout& layout, int s )
{
    return LbGeom { ( s % 2 ) * layout.tile_w, layout.tile_w, ( s / 2 ) * layout.tile_h, layout.tile_h };
}

TilePlacement place_tile( const StreamLayout& layout, const LbGeom& band, int src_idx )
{
    TilePlacement p;
    const bool multi = layout.nsrc > 1;
    p.cell = ( multi && src_idx >= 0 && src_idx < layout.nsrc ) ? src_idx : 0;
    const LbGeom src = ( band.w > 0 && band.h > 0 ) ? band : layout.band;
    const int tw = multi ? layout.tile_w : layout.band.w;
    const int th = multi ? layout.tile_h : layout.band.h;
    LbGeom dst = ( src.w == tw && src.h == th ) ? LbGeom { 0, tw, 0, th } : band_geom( src.w, src.h, tw, th );
    dst.left = ( ( p.cell % 2 ) * layout.tile_w + dst.left ) & ~1;
    dst.top = ( ( p.cell / 2 ) * layout.tile_h + dst.top ) & ~1;
    dst.w &= ~1;
    dst.h &= ~1;
    p.src = LbGeom { src.left & ~1, src.w & ~1, src.top & ~1, src.h & ~1 };
    p.dst = dst;
    return p;
}
