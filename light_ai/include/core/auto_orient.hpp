// Automatic camera orientation selector (0/90/180/270 deg) using EXIF hints
// and multi-angle YOLOv5 detection/aspect scoring. SDK-free, zero heap, noexcept.

#pragma once

#include <cstdint>

#include "core/detection.hpp"

namespace auto_orient
{
    int deg_to_idx( int deg ) noexcept;
    int idx_to_deg( int idx ) noexcept;
    void oriented_dims( int raw_w, int raw_h, int rot_deg, int& out_w, int& out_h ) noexcept;
    float score_detections( const DetList& dets ) noexcept;
}  // namespace auto_orient

class AutoOrient
{
public:
    enum class Action : uint8_t
    {
        None,
        ProbeAll,
        ProbeSingle
    };

    struct Result
    {
        bool changed = false;
        bool locked = false;
        int prev_rot = 0;
        int rot_deg = 0;
        float best_score = 0.0f;
        float second_score = 0.0f;
    };

    void reset( bool auto_enabled, int manual_rot_deg, bool raw_portrait ) noexcept;
    void set_exif_hint( int rot_deg ) noexcept;

    Action next_action( int frame_idx ) const noexcept;
    int watch_angle() const noexcept;
    bool enabled() const noexcept { return enabled_; }
    bool locked() const noexcept { return locked_; }
    int active_rot() const noexcept { return active_rot_; }

    // Commit scores obtained across all 4 angles {0, 90, 180, 270} on one frame.
    Result commit_all4( const float scores[4] ) noexcept;
    // Record a single-angle score (Watch probe or active frame); true = trigger ProbeAll.
    bool observe_single( int rot_deg, float score ) noexcept;

private:
    enum class Phase : uint8_t
    {
        Locked,
        Probe4,
        Watch
    };

    enum
    {
        kWatchInterval = 16,
        kMaxBurstRounds = 3,
        kVotesToLock = 2
    };

    void advance_watch_cursor() noexcept;
    void consume_burst_round() noexcept;
    Result apply_choice( int best_idx, float best_sc, float second_sc, bool lock_now ) noexcept;

private:
    Phase phase_ = Phase::Locked;
    bool enabled_ = false;
    bool locked_ = true;
    bool raw_portrait_ = false;
    int exif_rot_ = -1;
    int active_rot_ = 0;
    int burst_left_ = 0;
    int watch_cursor_ = 1;
    int votes_[4] {};
};
