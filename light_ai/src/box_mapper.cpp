#include <cstdint>

#include "box_mapper.h"

namespace {

void det_color( int cls, uint8_t& r, uint8_t& g, uint8_t& b )
{
    switch ( cls )
    {
        case 0:
            r = 255;
            g = 64;
            b = 64;
            break;  // person
        case 56:
            r = 64;
            g = 255;
            b = 64;
            break;  // chair
        default:
            r = 255;
            g = 255;
            b = 0;
            break;  // others
    }
}

void trail_color( int id, uint8_t& r, uint8_t& g, uint8_t& b )
{
    constexpr uint8_t pal[8][3] = {
        { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 }, { 255, 128, 0 },
        { 0, 255, 0 },   { 200, 0, 0 }, { 128, 128, 255 }, { 255, 64, 64 } };
    const int i = id & 7;
    r = pal[i][0];
    g = pal[i][1];
    b = pal[i][2];
}

}  // namespace

void fill_gpu_boxes( const DetList& dets, const TrajList& trajs, const Tracker& tracker,
                     GpuBoxList& boxes, const LbGeom& lb, float sx, float sy )
{
    boxes.clear();
    for ( int k = 0; k < dets.n; ++k )
    {
        const Detection& d = dets.d[k];
        GpuBox b{};
        b.x0 = d.x * sx + static_cast<float>( lb.left );
        b.y0 = d.y * sy + static_cast<float>( lb.top );
        b.x1 = ( d.x + d.w ) * sx + static_cast<float>( lb.left );
        b.y1 = ( d.y + d.h ) * sy + static_cast<float>( lb.top );
        // Clamp to the tensor bounds: the trail loop bounds-checks its
        // points and the GPU path clips silently, but draw_boxes_cpu
        // would draw an out-of-bounds box as a line squeezed onto the
        // edge -- keep both draw paths on the same geometry.
        if ( b.x1 <= static_cast<float>( lb.left ) || b.y1 <= static_cast<float>( lb.top )
             || b.x0 >= static_cast<float>( lb.left + lb.w ) || b.y0 >= static_cast<float>( lb.top + lb.h ) )
            continue;
        if ( b.x0 < static_cast<float>( lb.left ) )
            b.x0 = static_cast<float>( lb.left );
        if ( b.y0 < static_cast<float>( lb.top ) )
            b.y0 = static_cast<float>( lb.top );
        if ( b.x1 > static_cast<float>( lb.left + lb.w ) )
            b.x1 = static_cast<float>( lb.left + lb.w );
        if ( b.y1 > static_cast<float>( lb.top + lb.h ) )
            b.y1 = static_cast<float>( lb.top + lb.h );
        if ( b.x1 - b.x0 < 2.0f || b.y1 - b.y0 < 2.0f )
            continue;
        det_color( d.cls, b.r, b.g, b.b );
        b.a = 255;
        boxes.push( b );
    }
    if ( trajs.n > 0 )
    {
        // samp matches the dst_cap below; a smaller array with a
        // larger cap overflows the stack (segfault at pts>=61).
        TrajPoint samp[20];
        for ( int k = 0; k < trajs.n; ++k )
        {
            const TrackState& t = trajs.d[k];
            if ( t.state != TRACK_CONF )
                continue;  // trail after confirmation only
            const int ns = tracker.recent_points( t.id, 4, samp, 20 );
            float prev_px = 0.0f;
            float prev_py = 0.0f;
            bool have_prev = false;  // spatial dedup state (per track)
            for ( int j = 0; j < ns; ++j )
            {
                if ( boxes.n >= GpuBoxList::cap )  // PodList<GpuBox,128> cap
                    break;
                const float px = samp[j].x * sx + static_cast<float>( lb.left );
                const float py = samp[j].y * sy + static_cast<float>( lb.top );
                if ( px - 1.5f < static_cast<float>( lb.left ) || py - 1.5f < static_cast<float>( lb.top )
                     || px + 1.5f > static_cast<float>( lb.left + lb.w ) || py + 1.5f > static_cast<float>( lb.top + lb.h ) )
                    continue;
                if ( have_prev )
                {
                    const float dx = px - prev_px;
                    const float dy = py - prev_py;
                    if ( dx * dx + dy * dy < 16.0f )
                        continue;  // too close to the previous trail point
                }
                prev_px = px;
                prev_py = py;
                have_prev = true;
                GpuBox p{};
                p.x0 = px - 1.5f;
                p.y0 = py - 1.5f;
                p.x1 = px + 1.5f;
                p.y1 = py + 1.5f;
                trail_color( t.id, p.r, p.g, p.b );
                int a = 255 - ( ns - 1 - j ) * 10;  // newest opaque
                if ( a < 60 )
                    a = 60;
                p.a = static_cast<uint8_t>( a );
                boxes.push( p );
            }
        }
    }
}
