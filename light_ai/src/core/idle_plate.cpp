#include "core/idle_plate.hpp"

namespace
{
    const char* label()
    {
        return "NO SIGNAL";
    }
}  // namespace

int IdlePlate::text_w() const
{
    return font_.width( label(), kScale );
}

int IdlePlate::text_h() const
{
    return font_.height( label(), kScale );
}

int IdlePlate::text_x( const StreamLayout& layout ) const
{
    return ( layout.tile_w - text_w() ) / 2;
}

int IdlePlate::text_y( const StreamLayout& layout ) const
{
    return ( layout.tile_h - text_h() ) / 2;
}

void IdlePlate::draw( const Plane8& y_plane, const StreamLayout& layout )
{
    const int x0 = text_x( layout );
    const int y0 = text_y( layout );
    for ( int t = 0; t < layout.nsrc; ++t )
        font_.stamp( y_plane, ( t % 2 ) * layout.tile_w + x0, ( t / 2 ) * layout.tile_h + y0, label(), kScale, kLuma );
}
