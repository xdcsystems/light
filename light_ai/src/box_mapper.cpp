#include <algorithm>
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

// Shared box geometry for both draw paths: map one scene-space box into the
// letterbox band (scale + offset into model-input pixels), reject a box fully
// outside the band or thinner than the 2 px minimum, clamp the rest to the
// exact band bounds. The GPU path clips silently, but draw_boxes_cpu would
// draw an out-of-bounds box as a line squeezed onto the edge -- one geometry
// for det and track boxes, no second notion of "inside".
bool band_box( float x, float y, float w, float h,
               const LbGeom& lb, float sx, float sy, GpuBox& b )
{
    b.x0 = x * sx + static_cast<float>( lb.left );
    b.y0 = y * sy + static_cast<float>( lb.top );
    b.x1 = ( x + w ) * sx + static_cast<float>( lb.left );
    b.y1 = ( y + h ) * sy + static_cast<float>( lb.top );
    const float lb_left = static_cast<float>( lb.left );
    const float lb_top = static_cast<float>( lb.top );
    const float lb_right = static_cast<float>( lb.left + lb.w );
    const float lb_bottom = static_cast<float>( lb.top + lb.h );
    if ( b.x1 <= lb_left || b.y1 <= lb_top || b.x0 >= lb_right || b.y0 >= lb_bottom )
        return false;
    // The reject gate above keeps every coordinate one-sided inside the
    // band: the opposite clamp bound never fires, the values are the
    // same four one-sided fixes the explicit ifs used to produce.
    b.x0 = std::clamp( b.x0, lb_left, lb_right );
    b.y0 = std::clamp( b.y0, lb_top, lb_bottom );
    b.x1 = std::clamp( b.x1, lb_left, lb_right );
    b.y1 = std::clamp( b.y1, lb_top, lb_bottom );
    return b.x1 - b.x0 >= 2.0f && b.y1 - b.y0 >= 2.0f;
}

// Non-person det boxes from the detection list. Persons are drawn only from
// the Kalman posterior (push_track_box): an EMA detection box next to the
// track box showed two boxes per person, jitting independently of the trail
// (audit: double draw / box-trail mismatch).
void fill_det_boxes( const DetList& dets, GpuBoxList& boxes,
                     const LbGeom& lb, float sx, float sy )
{
    for ( int k = 0; k < dets.n; ++k )
    {
        const Detection& d = dets.d[k];
        GpuBox b {};
        if ( !band_box( d.x, d.y, d.w, d.h, lb, sx, sy, b ) )
            continue;
        if ( d.cls == 0 )
            continue;
        det_color( d.cls, b.r, b.g, b.b );
        b.a = 255;
        boxes.push( b );
    }
}

// Track box from the Kalman posterior: the single drawing source for persons
// after the EMA person boxes were removed (anti-jitter stage 1). TENT draws
// too -- a new person appears at once; the confirmation gate stays a
// stability concept, not a visibility gate. Same transform as the det boxes.
void push_track_box( const TrackState& t, GpuBoxList& boxes,
                     const LbGeom& lb, float sx, float sy )
{
    GpuBox tb {};
    if ( !band_box( t.cx - t.bw * 0.5f, t.cy - t.bh * 0.5f, t.bw, t.bh, lb, sx, sy, tb ) )
        return;
    if ( boxes.n >= GpuBoxList::cap )
        return;
    det_color( 0, tb.r, tb.g, tb.b );
    tb.a = 255;
    boxes.push( tb );
}

// Trail dots for CONF tracks and DEAD ghosts (see the state gate in
// fill_gpu_boxes). samp matches the dst_cap of recent_points below; a smaller
// array with a larger cap overflows the stack (segfault at pts>=61).
void fill_trail_points( const Tracker& tracker, const TrackState& t, int64_t now_us,
                        GpuBoxList& boxes, const LbGeom& lb, float sx, float sy )
{
    // stride 1: the ring is already thinned by the tracker-side
    // kTrajMinStepPx push gate, so every stored point is an honest
    // path vertex. The old stride 4 was a jitter-thinning hack for
    // per-frame pushes; over a sparse ring it both skips path
    // vertices (disconnected trail) and re-randomizes the sampled
    // subset on every new push (residual swarming).
    TrajPoint samp[20];
    const int ns = tracker.recent_points( t.id, 1, samp, 20 );

    // DEAD ghost box: fades with the trail instead of cutting
    // off at death. The age of the newest trail point proxies
    // the death time -- pushes stop at death, so the newest
    // point ages exactly like the box should fade. ns==0
    // (standing person, empty ring): the trail is already
    // invisible, the box goes with it.
    if ( t.state == TRACK_DEAD && ns > 0 )
    {
        int64_t age_us = now_us - samp[ns - 1].ts_us;
        if ( age_us < 0 )
            age_us = 0;
        // Box fade is capped at k_dead_box_fade_us: the trail is
        // a path history (where the person moved) and may
        // linger for kTrailMaxAgeMs, but a standing rectangle
        // reads as "a person is here" and must go quickly
        // once the track dies.
        // zero: the box vanishes the frame the track dies. A standing
        // rectangle reads as "a person is here" -- only the trail
        // is allowed to linger as path history.
        constexpr int64_t k_dead_box_fade_us = 0;
        const int64_t max_age_us =
            tracker.trail_max_age_us() < k_dead_box_fade_us
                ? tracker.trail_max_age_us()
                : k_dead_box_fade_us;
        if ( age_us < max_age_us )
        {
            GpuBox tb {};
            if ( band_box( t.cx - t.bw * 0.5f, t.cy - t.bh * 0.5f, t.bw, t.bh, lb, sx, sy, tb ) &&
                 boxes.n < GpuBoxList::cap )
            {
                det_color( 0, tb.r, tb.g, tb.b );
                int a = 255 - static_cast<int>( age_us * 255 / max_age_us );
                if ( a < 0 )
                    a = 0;
                tb.a = static_cast<uint8_t>( a );
                boxes.push( tb );
            }
        }
    }

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
        GpuBox p {};
        p.x0 = px - 1.5f;
        p.y0 = py - 1.5f;
        p.x1 = px + 1.5f;
        p.y1 = py + 1.5f;
        trail_color( t.id, p.r, p.g, p.b );
        // Age-based fade: newest point opaque, alpha falls linearly
        // with the point's age and reaches 0 exactly at the age the
        // tracker evicts it (kTrailMaxAgeMs). The old index-based
        // formula with a floor of 60 only approximated this while
        // pushes were per-frame; with the sparse gated pushes it
        // made stale points sit at the floor and then pop out
        // abruptly. No floor: fully transparent == already evicted.
        int64_t age_us = now_us - samp[j].ts_us;
        if ( age_us < 0 )
            age_us = 0;
        const int64_t max_age_us = tracker.trail_max_age_us();
        // fully transparent == evicted: the tracker evicts on a
        // strict-less-than, so a point exactly at max age survives
        // one frame with alpha 0 -- do not push that quad at all
        if ( age_us >= max_age_us )
            continue;
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
        // person's frozen window fades out over kTrailMaxAgeMs,
        // and evicted (invisible) tail points never pop.
        if ( ns > 1 )
            a = a * j / ( ns - 1 );
        p.a = static_cast<uint8_t>( a );
        // j == 0 lands here with a == 0 by design (the index ramp
        // maps the oldest drawn point to zero), but pushing an
        // invisible quad burns a VBO slot: with the 128-cap list
        // saturated, the freshest -- and brightest -- trail points
        // past the break are the ones that get dropped. Skip the
        // zero-alpha quads; monotonicity along the path is kept
        // (the next point already has a > 0).
        if ( p.a == 0 )
            continue;
        boxes.push( p );
    }
}

}  // namespace

void fill_gpu_boxes( const DetList& dets, const TrajList& trajs, const Tracker& tracker,
                     GpuBoxList& boxes, const LbGeom& lb, float sx, float sy,
                     int64_t now_us )
{
    boxes.clear();
    fill_det_boxes( dets, boxes, lb, sx, sy );
    for ( int k = 0; k < trajs.n; ++k )
    {
        const TrackState& t = trajs.d[k];
        if ( t.state == TRACK_TENT || t.state == TRACK_CONF )
            push_track_box( t, boxes, lb, sx, sy );
        // trail after confirmation; DEAD ghosts keep drawing too: their
        // points fade by age and the ring drains via the tracker-side
        // per-frame eviction, so a ghost trail self-terminates when its
        // youngest point ages out
        if ( t.state != TRACK_CONF && t.state != TRACK_DEAD )
            continue;
        fill_trail_points( tracker, t, now_us, boxes, lb, sx, sy );
    }
}
