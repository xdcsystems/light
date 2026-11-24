#include <algorithm>
#include <cmath>
#include <numeric>

#include "core/box_math.hpp"
#include "core/global_track.hpp"

namespace
{
    constexpr float kReidMargin = 0.06f;
    constexpr float kSwitchMargin = 0.10f;
    constexpr float kEmaAlpha = 0.20f;
    constexpr float kEmaRefreshExtra = 0.05f;
    constexpr float kMaxOtherIoU = 0.30f;
    constexpr float kClipHpRatio = 0.85f;
    constexpr int64_t kGlobalMaxAgeUs = 45LL * 1000 * 1000;
    constexpr int kUnboundRetryFrames = 5;

    struct CropBounds
    {
        float sx = 1.0f;
        float sy = 1.0f;
        int min_w = 16;
        int min_h = 32;
    };

    struct CropCand
    {
        ReidCropReq req {};
        bool need_init = false;
        int frames_since_reid = 0;
        int idx = 0;
    };

    bool any_cam_active( const GlobalTrack& g ) noexcept
    {
        return std::any_of( g.cams.begin(), g.cams.end(), []( const LocalBinding& b ) { return b.active; } );
    }

    bool collides_on_cam( const GlobalTrack& g, int src_idx, int local_id, uint32_t gen ) noexcept
    {
        const LocalBinding& b = g.cams[src_idx];
        return b.active && ( b.local_id != local_id || b.gen != gen );
    }

    bool has_live_track( const TrajList& trajs, int id, uint32_t gen ) noexcept
    {
        return std::any_of( trajs.d.begin(), trajs.d.begin() + trajs.n, [id, gen]( const TrackState& tr ) {
            return ( tr.state == TRACK_TENT || tr.state == TRACK_CONF ) && tr.id == id && tr.gen == gen;
        } );
    }

    float max_peer_iou( const TrajList& trajs, int self_k ) noexcept
    {
        const TrackState& a = trajs.d[self_k];
        float best = 0.0f;
        for ( int j = 0; j < trajs.n; ++j )
        {
            if ( j == self_k )
                continue;
            const TrackState& b = trajs.d[j];
            if ( b.state != TRACK_TENT && b.state != TRACK_CONF )
                continue;
            const float iou = box_iou( a.cx - a.bw * 0.5f, a.cy - a.bh * 0.5f, a.bw, a.bh,
                                       b.cx - b.bw * 0.5f, b.cy - b.bh * 0.5f, b.bw, b.bh );
            best = std::max( best, iou );
        }
        return best;
    }

    bool build_crop_rect( const TrackState& tr, const LbGeom& lb, const CropBounds& cb,
                          LbGeom& out, bool& clipped ) noexcept
    {
        if ( !std::isfinite( tr.cx ) || !std::isfinite( tr.cy ) ||
             !std::isfinite( tr.bw ) || !std::isfinite( tr.bh ) || tr.bw <= 0.0f || tr.bh <= 0.0f )
            return false;
        const float fl = static_cast<float>( lb.left );
        const float ft = static_cast<float>( lb.top );
        const float fr = static_cast<float>( lb.left + lb.w );
        const float fb = static_cast<float>( lb.top + lb.h );
        const int rx0 = static_cast<int>( std::clamp( std::floor( ( tr.cx - tr.bw * 0.5f ) * cb.sx + fl ), fl, fr ) );
        const int ry0 = static_cast<int>( std::clamp( std::floor( ( tr.cy - tr.bh * 0.5f ) * cb.sy + ft ), ft, fb ) );
        const int rx1 = static_cast<int>( std::clamp( std::ceil( ( tr.cx + tr.bw * 0.5f ) * cb.sx + fl ), fl, fr ) );
        const int ry1 = static_cast<int>( std::clamp( std::ceil( ( tr.cy + tr.bh * 0.5f ) * cb.sy + ft ), ft, fb ) );

        const int el = ( rx0 + 1 ) & ~1;
        const int et = ( ry0 + 1 ) & ~1;
        const int ew = ( rx1 & ~1 ) - el;
        const int eh = ( ry1 & ~1 ) - et;
        if ( ew < cb.min_w || eh < cb.min_h )
            return false;

        out = { el, ew, et, eh };
        const bool edge_touch = rx0 <= lb.left + 2 || ry0 <= lb.top + 2 || rx1 >= lb.left + lb.w - 2 || ry1 >= lb.top + lb.h - 2;
        clipped = edge_touch || ( tr.hp > 0.0f && tr.bh < kClipHpRatio * tr.hp );
        return true;
    }

    struct SimMatch
    {
        int best_slot = -1;
        float best_sim = -2.0f;
        float second_sim = -2.0f;
        float cur_sim = -2.0f;
    };

    SimMatch search_gallery( const std::array<GlobalTrack, reid_cfg::kMaxGlobalTracks>& tracks,
                             const ReidResult& res,
                             int cur_slot ) noexcept
    {
        SimMatch m {};
        for ( int s = 0; s < reid_cfg::kMaxGlobalTracks; ++s )
        {
            const GlobalTrack& g = tracks[s];
            if ( !g.used || g.dim != res.dim )
                continue;
            if ( collides_on_cam( g, res.src_idx, res.local_id, res.gen ) )
                continue;
            const float sim = dot_feature( res.feat.data(), g.ema_feat.data(), res.dim );
            if ( s == cur_slot )
                m.cur_sim = sim;
            if ( sim > m.best_sim )
            {
                m.second_sim = m.best_sim;
                m.best_sim = sim;
                m.best_slot = s;
                continue;
            }
            if ( sim > m.second_sim )
                m.second_sim = sim;
        }
        return m;
    }

}  // namespace

bool l2_normalize( float* vec, int dim ) noexcept
{
    if ( !vec || dim <= 0 )
        return false;
    if ( !std::all_of( vec, vec + dim, []( float x ) { return std::isfinite( x ); } ) )
        return false;
    const double sum = std::inner_product( vec, vec + dim, vec, 0.0, std::plus<>(), []( float x, float y ) {
        return static_cast<double>( x ) * static_cast<double>( y );
    } );
    if ( sum <= 1e-12 )
        return false;
    const float inv = static_cast<float>( 1.0 / std::sqrt( sum ) );
    std::transform( vec, vec + dim, vec, [inv]( float x ) { return x * inv; } );
    return true;
}

float dot_feature( const float* a, const float* b, int dim ) noexcept
{
    if ( !a || !b || dim <= 0 )
        return 0.0f;
    const double acc = std::inner_product( a, a + dim, b, 0.0, std::plus<>(), []( float x, float y ) {
        return static_cast<double>( x ) * static_cast<double>( y );
    } );
    return static_cast<float>( acc );
}

GlobalTrackTable::GlobalTrackTable( float sim_thr ) noexcept : sim_thr_( sim_thr )
{
}

void GlobalTrackTable::reset() noexcept
{
    tracks_.fill( GlobalTrack {} );
    for ( auto& row : live_ )
        row.fill( LiveLocal {} );
    next_global_id_ = 1;
}

int GlobalTrackTable::find_live_idx( int src_idx, int local_id, uint32_t gen ) const noexcept
{
    if ( src_idx < 0 || src_idx >= reid_cfg::kMaxSources )
        return -1;
    const auto& row = live_[src_idx];
    const auto* const it = std::find_if( row.begin(), row.end(), [local_id, gen]( const LiveLocal& lv ) {
        return lv.alive && lv.local_id == local_id && lv.gen == gen;
    } );
    return it != row.end() ? static_cast<int>( it - row.begin() ) : -1;
}

int GlobalTrackTable::find_global_by_local( int src_idx, int local_id, uint32_t gen ) const noexcept
{
    const auto* const it = std::find_if( tracks_.begin(), tracks_.end(), [src_idx, local_id, gen]( const GlobalTrack& g ) {
        if ( !g.used )
            return false;
        const LocalBinding& b = g.cams[src_idx];
        return b.active && b.local_id == local_id && b.gen == gen;
    } );
    return it != tracks_.end() ? static_cast<int>( it - tracks_.begin() ) : -1;
}

int GlobalTrackTable::alloc_global_slot() noexcept
{
    int oldest_inactive = -1;
    int64_t oldest_ts = 0;
    for ( int s = 0; s < reid_cfg::kMaxGlobalTracks; ++s )
    {
        if ( !tracks_[s].used )
            return s;
        if ( !any_cam_active( tracks_[s] ) && ( oldest_inactive < 0 || tracks_[s].last_seen_us < oldest_ts ) )
        {
            oldest_inactive = s;
            oldest_ts = tracks_[s].last_seen_us;
        }
    }
    return oldest_inactive;
}

void GlobalTrackTable::bind_slot( int gslot, int src_idx, int local_id, uint32_t gen, int64_t ts_us ) noexcept
{
    GlobalTrack& g = tracks_[gslot];
    g.cams[src_idx] = { local_id, gen, ts_us, true };
    g.last_seen_us = std::max( g.last_seen_us, ts_us );
}

void GlobalTrackTable::update_ema( GlobalTrack& g, const float* feat, int dim ) noexcept
{
    std::transform( g.ema_feat.begin(), g.ema_feat.begin() + dim, feat, g.ema_feat.begin(),
                    []( float old_v, float new_v ) { return ( 1.0f - kEmaAlpha ) * old_v + kEmaAlpha * new_v; } );
    if ( l2_normalize( g.ema_feat.data(), dim ) )
        ++g.samples;
}

void GlobalTrackTable::sync_global_entry( GlobalTrack& g, int src_idx, const TrajList& trajs, int64_t ts_us ) noexcept
{
    if ( !g.used )
        return;
    if ( g.cams[src_idx].active )
    {
        if ( has_live_track( trajs, g.cams[src_idx].local_id, g.cams[src_idx].gen ) )
        {
            g.cams[src_idx].last_seen_us = ts_us;
            g.last_seen_us = std::max( g.last_seen_us, ts_us );
        }
        else
        {
            g.cams[src_idx].active = false;
        }
    }
    if ( !any_cam_active( g ) && ( ts_us - g.last_seen_us ) > kGlobalMaxAgeUs )
        g = GlobalTrack {};
}

void GlobalTrackTable::sync_cam( int src_idx, TrajList& trajs, int64_t ts_us ) noexcept
{
    if ( src_idx < 0 || src_idx >= reid_cfg::kMaxSources )
        return;

    for ( GlobalTrack& g : tracks_ )
        sync_global_entry( g, src_idx, trajs, ts_us );

    std::array<LiveLocal, reid_cfg::kMaxLocalTracks> next {};
    int out_n = 0;
    for ( int k = 0; k < trajs.n && out_n < reid_cfg::kMaxLocalTracks; ++k )
    {
        TrackState& tr = trajs.d[k];
        if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
            continue;
        const int prev = find_live_idx( src_idx, tr.id, tr.gen );
        LiveLocal item = ( prev >= 0 ) ? live_[src_idx][prev] : LiveLocal {};
        item.alive = true;
        item.local_id = tr.id;
        item.gen = tr.gen;
        item.frames_since_reid = ( prev >= 0 ) ? ( item.frames_since_reid + 1 ) : reid_cfg::kReidRefreshFrames;
        const int gslot = find_global_by_local( src_idx, tr.id, tr.gen );
        item.global_slot = gslot;
        tr.global_id = ( gslot >= 0 ) ? tracks_[gslot].global_id : 0;
        next[out_n++] = item;
    }
    live_[src_idx] = next;
}

void GlobalTrackTable::refresh_global_ids( int src_idx, TrajList& trajs ) const noexcept
{
    if ( src_idx < 0 || src_idx >= reid_cfg::kMaxSources )
        return;
    for ( int k = 0; k < trajs.n; ++k )
    {
        TrackState& tr = trajs.d[k];
        if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
            continue;
        const int gslot = find_global_by_local( src_idx, tr.id, tr.gen );
        tr.global_id = ( gslot >= 0 ) ? tracks_[gslot].global_id : 0;
    }
}

void GlobalTrackTable::apply_bound_result( const ReidResult& res, LiveLocal& lv, int cur_slot ) noexcept
{
    const SimMatch m = search_gallery( tracks_, res, cur_slot );
    // Switch only when current affinity drops below threshold and another gallery entry clears sim_thr_.
    const bool cur_weak = m.cur_sim < ( sim_thr_ - 0.05f );
    const bool margin_ok = m.second_sim < -1.0f || ( m.best_sim - m.second_sim ) >= kReidMargin;
    if ( cur_weak && margin_ok && m.best_slot >= 0 && m.best_slot != cur_slot &&
         m.best_sim >= sim_thr_ && m.best_sim > m.cur_sim + kSwitchMargin )
    {
        const int cand_gid = tracks_[m.best_slot].global_id;
        lv.pending_switch_hits = ( lv.pending_switch_gid == cand_gid ) ? ( lv.pending_switch_hits + 1 ) : 1;
        lv.pending_switch_gid = cand_gid;
        if ( lv.pending_switch_hits >= reid_cfg::kSwitchConfirmHits )
        {
            tracks_[cur_slot].cams[res.src_idx].active = false;
            bind_slot( m.best_slot, res.src_idx, res.local_id, res.gen, res.ts_us );
            lv.global_slot = m.best_slot;
            lv.pending_switch_gid = 0;
            lv.pending_switch_hits = 0;
            update_ema( tracks_[m.best_slot], res.feat.data(), res.dim );
        }
        return;
    }
    lv.pending_switch_gid = 0;
    lv.pending_switch_hits = 0;
    if ( tracks_[cur_slot].samples == 0 || m.cur_sim >= sim_thr_ + kEmaRefreshExtra )
        update_ema( tracks_[cur_slot], res.feat.data(), res.dim );
}

bool GlobalTrackTable::apply_unbound_result( const ReidResult& res, LiveLocal& lv ) noexcept
{
    const SimMatch m = search_gallery( tracks_, res, -1 );
    const bool margin_ok = m.second_sim < -1.0f || ( m.best_sim - m.second_sim ) >= kReidMargin;
    if ( m.best_slot >= 0 && m.best_sim >= sim_thr_ )
    {
        if ( !margin_ok )
            return false;
        bind_slot( m.best_slot, res.src_idx, res.local_id, res.gen, res.ts_us );
        lv.global_slot = m.best_slot;
        if ( m.best_sim >= sim_thr_ + kEmaRefreshExtra )
            update_ema( tracks_[m.best_slot], res.feat.data(), res.dim );
        return true;
    }

    const int nslot = alloc_global_slot();
    if ( nslot < 0 )
        return false;
    GlobalTrack& g = tracks_[nslot];
    g = GlobalTrack {};
    g.used = true;
    g.global_id = next_global_id_++;
    g.birth_us = res.ts_us;
    g.last_seen_us = res.ts_us;
    g.dim = res.dim;
    g.samples = 1;
    std::copy_n( res.feat.data(), res.dim, g.ema_feat.data() );
    g.cams[res.src_idx] = { res.local_id, res.gen, res.ts_us, true };
    lv.global_slot = nslot;
    return false;
}

bool GlobalTrackTable::apply_result( const ReidResult& res ) noexcept
{
    if ( !res.ok || res.src_idx < 0 || res.src_idx >= reid_cfg::kMaxSources || res.dim <= 0 ||
         res.dim > reid_cfg::kReidDimMax )
        return false;

    const int lidx = find_live_idx( res.src_idx, res.local_id, res.gen );
    if ( lidx < 0 )
        return false;

    LiveLocal& lv = live_[res.src_idx][lidx];
    const int cur_slot = find_global_by_local( res.src_idx, res.local_id, res.gen );
    if ( cur_slot >= 0 )
    {
        apply_bound_result( res, lv, cur_slot );
        return false;
    }
    return apply_unbound_result( res, lv );
}

bool GlobalTrackTable::try_pick_crop( int src_idx,
                                      const TrajList& trajs,
                                      int k,
                                      const LbGeom& lb,
                                      float sx,
                                      float sy,
                                      int min_w,
                                      int min_h,
                                      bool& need_init,
                                      int& frames_since_reid,
                                      ReidCropReq& out ) const noexcept
{
    const TrackState& tr = trajs.d[k];
    if ( tr.state != TRACK_CONF )
        return false;
    const int lidx = find_live_idx( src_idx, tr.id, tr.gen );
    if ( lidx < 0 )
        return false;
    const LiveLocal& lv = live_[src_idx][lidx];
    need_init = ( lv.global_slot < 0 );
    frames_since_reid = lv.frames_since_reid;
    const int interval = need_init
                               ? kUnboundRetryFrames
                               : ( ( tracks_[lv.global_slot].samples >= reid_cfg::kReidStableSamples )
                                         ? reid_cfg::kReidStableFrames
                                         : reid_cfg::kReidRefreshFrames );
    if ( lv.frames_since_reid < interval )
        return false;

    LbGeom srect {};
    bool clipped = false;
    if ( !build_crop_rect( tr, lb, { sx, sy, min_w, min_h }, srect, clipped ) )
        return false;
    const bool occluded = max_peer_iou( trajs, k ) >= kMaxOtherIoU;
    if ( clipped || occluded )
        return false;
    out = { tr.id, tr.gen, srect };
    return true;
}

int GlobalTrackTable::select_crops( int src_idx,
                                    const TrajList& trajs,
                                    const LbGeom& lb,
                                    int src_w,
                                    int src_h,
                                    int dst_w,
                                    int dst_h,
                                    ReidCropReq* dst,
                                    int max_req ) const noexcept
{
    if ( !dst || max_req <= 0 || src_idx < 0 || src_idx >= reid_cfg::kMaxSources ||
         src_w <= 0 || src_h <= 0 || lb.w <= 0 || lb.h <= 0 || dst_w <= 0 || dst_h <= 0 )
        return 0;

    const float sx = static_cast<float>( lb.w ) / static_cast<float>( src_w );
    const float sy = static_cast<float>( lb.h ) / static_cast<float>( src_h );
    const int cap = std::min( max_req, static_cast<int>( reid_cfg::kMaxCropsPerFrame ) );
    const int min_w = ( dst_w + 7 ) / 8;
    const int min_h = ( dst_h + 7 ) / 8;

    std::array<CropCand, TrajList::cap> cands {};
    int n_cands = 0;
    for ( int k = 0; k < trajs.n && n_cands < TrajList::cap; ++k )
    {
        CropCand cand {};
        cand.idx = k;
        if ( try_pick_crop( src_idx, trajs, k, lb, sx, sy, min_w, min_h, cand.need_init, cand.frames_since_reid, cand.req ) )
            cands[n_cands++] = cand;
    }

    const int picked = std::min( cap, n_cands );
    const auto cmp = []( const CropCand& a, const CropCand& b ) {
        if ( a.need_init != b.need_init )
            return a.need_init;
        if ( a.frames_since_reid != b.frames_since_reid )
            return a.frames_since_reid > b.frames_since_reid;
        return a.idx < b.idx;
    };
    if ( n_cands > picked )
        std::partial_sort( cands.begin(), cands.begin() + picked, cands.begin() + n_cands, cmp );
    else
        std::sort( cands.begin(), cands.begin() + n_cands, cmp );

    for ( int i = 0; i < picked; ++i )
        dst[i] = cands[i].req;
    return picked;
}

void GlobalTrackTable::mark_dispatched( int src_idx, int local_id, uint32_t gen ) noexcept
{
    const int idx = find_live_idx( src_idx, local_id, gen );
    if ( idx >= 0 )
        live_[src_idx][idx].frames_since_reid = 0;
}

int GlobalTrackTable::active_global_count() const noexcept
{
    return static_cast<int>( std::count_if( tracks_.begin(), tracks_.end(), []( const GlobalTrack& g ) {
        return g.used;
    } ) );
}
