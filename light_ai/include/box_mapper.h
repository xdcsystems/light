// Scene -> tensor coordinate mapping for detection boxes and track trails.
// Sole consumer: OverlaySink::draw_and_publish (a member there; the GPU and
// CPU draw paths share the same mapped batch). One geometry and one palette
// for both draw paths.

#pragma once

#include <array>
#include <cstdint>

#include "core/detection.hpp"  // DetList, Detection
#include "core/geometry.hpp"   // LbGeom
#include "core/gpu_box.hpp"    // GpuBox, GpuBoxList
#include "tracker.hpp"         // TrajList, TrackState, Tracker

// Detection boxes and trajectory trails -> boxes (tensor coords).
// Trail points are sampled ring points as 3x3 px GpuBoxes,
// in the same batch as the detection boxes.
// Scene->tensor mapping is (s, lb origin) -- identical for both loops.
// Detection boxes win the VBO budget (GpuBoxList::cap).
//
// trajs: the display tracks (boxes through BoxFilter); post: the same list as the
// tracker produced it (Kalman posteriors, the coordinates the trail was recorded
// in), entry k of both the same track. The trail head of a TENT / CONF track is
// bent onto its displayed box: the newest point moves by the full box offset
// (display center - posterior center), the shift fades out linearly over the
// kTrailBlend newest points, older points stay where they were recorded. With
// post == trajs (no display filter) the trail is the recorded one.
//
// now_us: current frame timestamp, drives the per-point alpha fade:
// newest point opaque, fades to 0 over the tracker's trail_max_age_us() --
// matching the tracker-side eviction, so a point disappears from the batch
// the same moment its alpha would reach zero.
//
// Threads: the thread that calls OverlaySink::draw_and_publish (the
// pipeline thread); the trail sample buffer is a member.
class BoxMapper
{
public:
    enum
    {
        kTrailSamples = 20,  // trail points read per track (the recent_points dst_cap)
        kTrailBlend = 6,     // newest trail points bent onto the displayed box
    };

    // Fills boxes (cleared at entry) from the detections and the tracks.
    void fill( const DetList& dets, const TrajList& trajs, const TrajList& post, const Tracker& tracker,
               GpuBoxList& boxes, const LbGeom& lb, float sx, float sy, int64_t now_us ) noexcept;

private:
    // The letterbox band in tensor pixels, with the scene -> tensor scale.
    struct Band
    {
        float left, top, right, bottom;
        float sx, sy;
    };
    // The spatial dedup state of one track's trail.
    struct PrevDot
    {
        float x = 0.0f;
        float y = 0.0f;
        bool set = false;
    };

    bool band_box( float x, float y, float w, float h, const Band& b, GpuBox& out ) const noexcept;
    void det_boxes( const DetList& dets, const Band& b, GpuBoxList& boxes ) const noexcept;
    void track_box( const TrackState& t, const Band& b, GpuBoxList& boxes ) const noexcept;
    // The scene-pixel shift of the newest trail point: display box - posterior (0 when not drawn as a box).
    struct Shift
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    Shift head_shift( const TrackState& t, const TrajList& post, int k ) const noexcept;
    void trail_points( const Tracker& tracker, const TrackState& t, const Shift& sh, int64_t now_us, const Band& b,
                       GpuBoxList& boxes ) noexcept;
    bool trail_dot( const TrajPoint& pt, const Band& b, PrevDot& prev, float& px, float& py ) const noexcept;
    int trail_alpha( int64_t age_us, int64_t max_age_us, int j, int ns ) const noexcept;
    void det_color( int cls, GpuBox& b ) const noexcept;
    void trail_color( int id, GpuBox& b ) const noexcept;

private:
    std::array<TrajPoint, kTrailSamples> samp_ {};  // one track's trail sample, refilled per track
};
