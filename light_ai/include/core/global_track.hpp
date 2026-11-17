// Cross-camera person Re-ID gallery and crop scheduler (SDK-free, zero heap, noexcept).
// Runs entirely on the pipeline thread: Tracker keeps per-camera local IDs for
// trajectories, while GlobalTrackTable syncs TrackState::global_id across cameras.

#pragma once

#include <array>
#include <cstdint>

#include "core/geometry.hpp"  // LbGeom
#include "core/track.hpp"     // TrajList, TrackState

namespace reid_cfg
{
    enum
    {
        kMaxSources = 4,
        kMaxLocalTracks = TrajList::cap,
        kMaxGlobalTracks = 32,
        kReidDimMax = 512,
        kReidSlots = 4,
        kReidQueueCap = 4,
        kSwitchConfirmHits = 2,
        kReidRefreshFrames = 15,
        kReidStableFrames = 30,
        kReidStableSamples = 5,
        kMaxCropsPerFrame = 2
    };
}

struct ReidJob
{
    int slot = -1;
    int src_idx = 0;
    int local_id = 0;
    uint32_t gen = 0;
    int64_t ts_us = 0;
    bool clipped = false;
    bool occluded = false;
};

struct ReidResult
{
    int src_idx = 0;
    int local_id = 0;
    uint32_t gen = 0;
    int64_t ts_us = 0;
    int dim = 0;
    bool clipped = false;
    bool occluded = false;
    bool ok = false;
    std::array<float, reid_cfg::kReidDimMax> feat {};
};

struct ReidCropReq
{
    int local_id = 0;
    uint32_t gen = 0;
    LbGeom srect {};
    bool clipped = false;
    bool occluded = false;
};

struct LocalBinding
{
    int local_id = 0;
    uint32_t gen = 0;
    int64_t last_seen_us = 0;
    bool active = false;
};

struct GlobalTrack
{
    bool used = false;
    int global_id = 0;
    int64_t birth_us = 0;
    int64_t last_seen_us = 0;
    int dim = 0;
    int samples = 0;
    std::array<float, reid_cfg::kReidDimMax> ema_feat {};
    std::array<LocalBinding, reid_cfg::kMaxSources> cams {};
};

// In-place L2 normalization; returns false on zero/non-finite vector.
bool l2_normalize( float* vec, int dim ) noexcept;

// Inner product of two unit feature vectors (cosine similarity).
float dot_feature( const float* a, const float* b, int dim ) noexcept;

class GlobalTrackTable
{
    struct LiveLocal
    {
        bool alive = false;
        int local_id = 0;
        uint32_t gen = 0;
        int global_slot = -1;
        int pending_switch_gid = 0;
        int pending_switch_hits = 0;
        int frames_since_reid = 0;
    };

public:
    explicit GlobalTrackTable( float sim_thr = 0.62f ) noexcept;

    void set_sim_thr( float thr ) noexcept { sim_thr_ = thr; }
    float sim_thr() const noexcept { return sim_thr_; }
    void reset() noexcept;

    // Synchronizes camera `src_idx`: deactivates vanished tracks, evicts expired
    // global tracks (> 45 s), and writes TrackState::global_id onto live trajs.
    void sync_cam( int src_idx, TrajList& trajs, int64_t ts_us ) noexcept;

    // Applies an asynchronous Re-ID inference result with collision guard,
    // margin check, switch hysteresis, and clipped/occluded EMA gating.
    void apply_result( const ReidResult& res ) noexcept;

    // Selects up to max_req confirmed track crops mapped to tensor pixels (even-aligned,
    // within lb, and at least 1/8 of dst_w x dst_h to obey RGA3/RGA2 8x upscale limit).
    int select_crops( int src_idx,
                      const TrajList& trajs,
                      const LbGeom& lb,
                      int src_w,
                      int src_h,
                      int dst_w,
                      int dst_h,
                      ReidCropReq* dst,
                      int max_req ) const noexcept;

    // Resets refresh counter only after RGA crop + queue submission succeeds.
    void mark_dispatched( int src_idx, int local_id, uint32_t gen ) noexcept;

    int active_global_count() const noexcept;

private:
    int find_live_idx( int src_idx, int local_id, uint32_t gen ) const noexcept;
    int find_global_by_local( int src_idx, int local_id, uint32_t gen ) const noexcept;
    int alloc_global_slot() noexcept;
    void bind_slot( int gslot, int src_idx, int local_id, uint32_t gen, int64_t ts_us ) noexcept;
    void update_ema( GlobalTrack& g, const float* feat, int dim ) noexcept;
    void sync_global_entry( GlobalTrack& g, int src_idx, const TrajList& trajs, int64_t ts_us ) noexcept;
    void apply_bound_result( const ReidResult& res, LiveLocal& lv, int cur_slot ) noexcept;
    void apply_unbound_result( const ReidResult& res, LiveLocal& lv ) noexcept;
    bool try_pick_crop( int src_idx,
                        const TrajList& trajs,
                        int k,
                        const LbGeom& lb,
                        float sx,
                        float sy,
                        int min_w,
                        int min_h,
                        bool need_init_pass,
                        ReidCropReq& out ) const noexcept;

private:
    std::array<GlobalTrack, reid_cfg::kMaxGlobalTracks> tracks_ {};
    std::array<std::array<LiveLocal, reid_cfg::kMaxLocalTracks>, reid_cfg::kMaxSources> live_ {};
    int next_global_id_ = 1;
    float sim_thr_ = 0.62f;
};
