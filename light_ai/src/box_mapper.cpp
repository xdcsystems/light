#include <algorithm>
#include <cstdint>

#include "box_mapper.h"

void BoxMapper::fill( const DetList& dets, const TrajList& trajs, const Tracker& tracker,
                      GpuBoxList& boxes, const LbGeom& lb, float sx, float sy, int64_t now_us ) noexcept
{
    boxes.clear();
    const Band b { static_cast<float>( lb.left ), static_cast<float>( lb.top ), static_cast<float>( lb.left + lb.w ),
                   static_cast<float>( lb.top + lb.h ), sx, sy };
    det_boxes( dets, b, boxes );
    for ( int k = 0; k < trajs.n; ++k )
    {
        const TrackState& t = trajs.d[k];
        if ( t.state == TRACK_TENT || t.state == TRACK_CONF )
            track_box( t, b, boxes );
    }
    for ( int k = 0; k < trajs.n; ++k )
    {
        const TrackState& t = trajs.d[k];
        // trail after confirmation; DEAD ghosts keep drawing too: their
        // points fade by age and the ring drains via the tracker-side
        // per-frame eviction, so a ghost trail self-terminates when its
        // youngest point ages out
        if ( t.state == TRACK_CONF || t.state == TRACK_DEAD )
            trail_points( tracker, t, now_us, b, boxes );
    }
}

// Shared box geometry for both draw paths: map one scene-space box into the
// letterbox band (scale + offset into model-input pixels), reject a box fully
// outside the band or thinner than the 2 px minimum, clamp the rest to the
// exact band bounds. The GPU path clips silently, but draw_boxes_cpu would
// draw an out-of-bounds box as a line squeezed onto the edge -- one geometry
// for det and track boxes, no second notion of "inside".
bool BoxMapper::band_box( float x, float y, float w, float h, const Band& b, GpuBox& out ) const noexcept
{
    out.x0 = x * b.sx + b.left;
    out.y0 = y * b.sy + b.top;
    out.x1 = ( x + w ) * b.sx + b.left;
    out.y1 = ( y + h ) * b.sy + b.top;
    if ( out.x1 <= b.left || out.y1 <= b.top || out.x0 >= b.right || out.y0 >= b.bottom )
        return false;
    // The reject gate above keeps every coordinate one-sided inside the
    // band: the opposite clamp bound never fires, the values are the
    // same four one-sided fixes the explicit ifs used to produce.
    out.x0 = std::clamp( out.x0, b.left, b.right );
    out.y0 = std::clamp( out.y0, b.top, b.bottom );
    out.x1 = std::clamp( out.x1, b.left, b.right );
    out.y1 = std::clamp( out.y1, b.top, b.bottom );
    return out.x1 - out.x0 >= 2.0f && out.y1 - out.y0 >= 2.0f;
}

// Non-person det boxes from the detection list. Persons are drawn only from
// the Kalman posterior (track_box): an EMA detection box next to the
// track box showed two boxes per person, jitting independently of the trail
// (audit: double draw / box-trail mismatch).
void BoxMapper::det_boxes( const DetList& dets, const Band& b, GpuBoxList& boxes ) const noexcept
{
    for ( int k = 0; k < dets.n; ++k )
    {
        const Detection& d = dets.d[k];
        GpuBox gb {};
        if ( !band_box( d.x, d.y, d.w, d.h, b, gb ) )
            continue;
        if ( d.cls == 0 )
            continue;
        det_color( d.cls, gb );
        gb.a = 255;
        boxes.push( gb );
    }
}

// Track box from the Kalman posterior: the single drawing source for persons
// after the EMA person boxes were removed (anti-jitter stage 1). TENT draws
// too -- a new person appears at once; the confirmation gate stays a
// stability concept, not a visibility gate. Same transform as the det boxes.
// A DEAD track draws no box: a standing rectangle reads as "a person is
// here" -- only the trail is allowed to linger as path history.
void BoxMapper::track_box( const TrackState& t, const Band& b, GpuBoxList& boxes ) const noexcept
{
    GpuBox tb {};
    if ( !band_box( t.cx - t.bw * 0.5f, t.cy - t.bh * 0.5f, t.bw, t.bh, b, tb ) )
        return;
    if ( boxes.n >= GpuBoxList::cap )
        return;
    det_color( 0, tb );
    tb.a = 255;
    boxes.push( tb );
}

// Trail dots for CONF tracks and DEAD ghosts (see the state gate in fill).
void BoxMapper::trail_points( const Tracker& tracker, const TrackState& t, int64_t now_us, const Band& b, GpuBoxList& boxes ) noexcept
{
    // stride 1: the ring is already thinned by the tracker-side
    // step gate (TrailWindow), so every stored point is an honest
    // path vertex. The old stride 4 was a jitter-thinning hack for
    // per-frame pushes; over a sparse ring it both skips path
    // vertices (disconnected trail) and re-randomizes the sampled
    // subset on every new push (residual swarming).
    const int ns = tracker.recent_points( t.id, 1, samp_.data(), kTrailSamples );
    const int64_t max_age_us = tracker.trail_max_age_us();
    PrevDot prev {};
    for ( int j = 0; j < ns; ++j )
    {
        if ( boxes.n >= GpuBoxList::cap )  // PodList<GpuBox,128> cap
            break;
        float px = 0.0f;
        float py = 0.0f;
        if ( !trail_dot( samp_[j], b, prev, px, py ) )
            continue;
        const int a = trail_alpha( now_us - samp_[j].ts_us, max_age_us, j, ns );
        // j == 0 lands here with a == 0 by design (the index ramp
        // maps the oldest drawn point to zero), but pushing an
        // invisible quad burns a VBO slot: with the 128-cap list
        // saturated, the freshest -- and brightest -- trail points
        // past the break are the ones that get dropped. Skip the
        // zero-alpha quads; monotonicity along the path is kept
        // (the next point already has a > 0).
        if ( a == 0 )
            continue;
        GpuBox p {};
        p.x0 = px - 1.5f;
        p.y0 = py - 1.5f;
        p.x1 = px + 1.5f;
        p.y1 = py + 1.5f;
        trail_color( t.display_id(), p );
        p.a = static_cast<uint8_t>( a );
        boxes.push( p );
    }
}

// One trail point in tensor pixels (px, py), or false: a 3x3 dot that does
// not fit the band, or one closer than 4 px to the previous drawn dot of the
// track. A dot that passes becomes the new previous one -- even when its alpha
// later turns out to be zero.
bool BoxMapper::trail_dot( const TrajPoint& pt, const Band& b, PrevDot& prev, float& px, float& py ) const noexcept
{
    px = pt.x * b.sx + b.left;
    py = pt.y * b.sy + b.top;
    if ( px - 1.5f < b.left || py - 1.5f < b.top || px + 1.5f > b.right || py + 1.5f > b.bottom )
        return false;
    if ( prev.set )
    {
        const float dx = px - prev.x;
        const float dy = py - prev.y;
        if ( dx * dx + dy * dy < 16.0f )
            return false;  // too close to the previous trail point
    }
    prev.x = px;
    prev.y = py;
    prev.set = true;
    return true;
}

// Age-based fade: newest point opaque, alpha falls linearly
// with the point's age and reaches 0 exactly at the age the
// tracker evicts it (trail_max_age_us). The old index-based
// formula with a floor of 60 only approximated this while
// pushes were per-frame; with the sparse gated pushes it
// made stale points sit at the floor and then pop out
// abruptly. No floor: fully transparent == already evicted.
// Returns 0..255; 0 = do not draw.
int BoxMapper::trail_alpha( int64_t age_us, int64_t max_age_us, int j, int ns ) const noexcept
{
    if ( age_us < 0 )
        age_us = 0;
    // fully transparent == evicted: the tracker evicts on a
    // strict-less-than, so a point exactly at max age survives
    // one frame with alpha 0 -- do not push that quad at all
    if ( age_us >= max_age_us )
        return 0;
    int a = std::clamp( 255 - static_cast<int>( age_us * 255 / max_age_us ), 0, 255 );
    // Spatial coherence: pure chronological alpha is monotone
    // in time but not along the drawn path -- when the path
    // retraces itself, an old dim dot sits next to a fresh
    // bright one and the fade reads as shuffled. Blend with an
    // index ramp over the drawn sample (oldest drawn -> 0,
    // newest -> untouched): both factors are monotone in j
    // (the ring is served oldest-first), so the product is
    // monotone along the path by construction. The age term
    // still drives the global fade-to-zero: a standing
    // person's frozen window fades out over trail_max_age_us,
    // and evicted (invisible) tail points never pop.
    if ( ns > 1 )
        a = a * j / ( ns - 1 );
    return a;
}

void BoxMapper::det_color( int cls, GpuBox& b ) const noexcept
{
    switch ( cls )
    {
        case 0:
            b.r = 255;
            b.g = 64;
            b.b = 64;
            break;  // person
        case 56:
            b.r = 64;
            b.g = 255;
            b.b = 64;
            break;  // chair
        default:
            b.r = 255;
            b.g = 255;
            b.b = 0;
            break;  // others
    }
}

void BoxMapper::trail_color( int id, GpuBox& b ) const noexcept
{
    constexpr uint8_t pal[8][3] = {
        { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 }, { 255, 128, 0 },
        { 0, 255, 0 },   { 200, 0, 0 }, { 128, 128, 255 }, { 255, 64, 64 } };
    const int i = id & 7;
    b.r = pal[i][0];
    b.g = pal[i][1];
    b.b = pal[i][2];
}
