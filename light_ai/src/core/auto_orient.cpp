#include <algorithm>
#include <array>

#include "core/auto_orient.hpp"

namespace
{
    constexpr float kMinConf = 0.25f;
    constexpr float kSignalMin = 0.45f;
    constexpr float kStrongSignal = 1.20f;
    constexpr float kSingleObserveTrigger = 0.45f;
    constexpr float kExifTieBoost = 0.35f;

    bool is_vertical_coco( int cls ) noexcept
    {
        switch ( cls )
        {
            case 0:   // person
            case 39:  // bottle
            case 40:  // wine glass
            case 41:  // cup
            case 56:  // chair
            case 58:  // potted plant
            case 72:  // refrigerator
            case 75:  // vase
                return true;
            default:
                return false;
        }
    }

    bool is_horizontal_coco( int cls ) noexcept
    {
        switch ( cls )
        {
            case 57:  // couch
            case 59:  // bed
            case 60:  // dining table
            case 62:  // tv
            case 63:  // laptop
            case 66:  // keyboard
                return true;
            default:
                return false;
        }
    }

    float aspect_weight( const Detection& d ) noexcept
    {
        if ( d.w <= 1.0f || d.h <= 1.0f )
            return 0.0f;
        if ( is_vertical_coco( d.cls ) )
        {
            if ( d.h >= d.w * 1.05f )
                return d.conf * 0.35f;
            if ( d.w > d.h * 1.45f )
                return -d.conf * 0.20f;
        }
        else if ( is_horizontal_coco( d.cls ) && d.w >= d.h * 1.05f )
        {
            return d.conf * 0.25f;
        }
        return 0.0f;
    }

    void find_top_two( const std::array<float, 4>& adj, int& best_idx, float& best_sc, float& second_sc ) noexcept
    {
        best_idx = 0;
        best_sc = adj[0];
        second_sc = 0.0f;
        for ( int i = 1; i < 4; ++i )
        {
            if ( adj[i] > best_sc )
            {
                second_sc = best_sc;
                best_sc = adj[i];
                best_idx = i;
            }
            else if ( adj[i] > second_sc )
            {
                second_sc = adj[i];
            }
        }
    }

}  // namespace

namespace auto_orient
{

int deg_to_idx( int deg ) noexcept
{
    switch ( deg )
    {
        case 90:
            return 1;
        case 180:
            return 2;
        case 270:
            return 3;
        default:
            return 0;
    }
}

int idx_to_deg( int idx ) noexcept
{
    switch ( idx & 3 )
    {
        case 1:
            return 90;
        case 2:
            return 180;
        case 3:
            return 270;
        default:
            return 0;
    }
}

void oriented_dims( int raw_w, int raw_h, int rot_deg, int& out_w, int& out_h ) noexcept
{
    const bool swap = ( rot_deg == 90 || rot_deg == 270 );
    out_w = swap ? raw_h : raw_w;
    out_h = swap ? raw_w : raw_h;
}

float score_detections( const DetList& dets ) noexcept
{
    float sum = 0.0f;
    int n_valid = 0;
    for ( int i = 0; i < dets.n; ++i )
    {
        const Detection& d = dets.d[i];
        if ( d.conf < kMinConf )
            continue;
        ++n_valid;
        if ( n_valid <= 8 )
            sum += d.conf + aspect_weight( d );
    }
    if ( n_valid == 0 )
        return 0.0f;
    const float bonus = 0.10f * static_cast<float>( std::min( n_valid, 6 ) );
    return std::max( 0.0f, sum + bonus );
}

}  // namespace auto_orient

void AutoOrient::reset( bool auto_enabled, int manual_rot_deg, bool raw_portrait ) noexcept
{
    std::fill( std::begin( votes_ ), std::end( votes_ ), 0 );
    raw_portrait_ = raw_portrait;
    exif_rot_ = -1;
    enabled_ = auto_enabled;
    if ( !auto_enabled )
    {
        phase_ = Phase::Locked;
        locked_ = true;
        active_rot_ = auto_orient::idx_to_deg( auto_orient::deg_to_idx( manual_rot_deg ) );
        burst_left_ = 0;
        watch_cursor_ = 1;
        return;
    }
    phase_ = Phase::Probe4;
    locked_ = false;
    active_rot_ = 0;
    burst_left_ = kMaxBurstRounds;
    watch_cursor_ = 1;
}

void AutoOrient::set_exif_hint( int rot_deg ) noexcept
{
    if ( !enabled_ || locked_ || exif_rot_ >= 0 )
        return;
    if ( rot_deg != 0 && rot_deg != 90 && rot_deg != 180 && rot_deg != 270 )
        return;
    exif_rot_ = rot_deg;
    if ( burst_left_ == kMaxBurstRounds )
        active_rot_ = rot_deg;
}

AutoOrient::Action AutoOrient::next_action( int frame_idx ) const noexcept
{
    if ( !enabled_ || locked_ || phase_ == Phase::Locked )
        return Action::None;
    if ( phase_ == Phase::Probe4 )
        return Action::ProbeAll;
    if ( frame_idx > 0 && ( frame_idx % kWatchInterval ) == 0 )
        return Action::ProbeSingle;
    return Action::None;
}

int AutoOrient::watch_angle() const noexcept
{
    return auto_orient::idx_to_deg( watch_cursor_ );
}

void AutoOrient::advance_watch_cursor() noexcept
{
    const int active_idx = auto_orient::deg_to_idx( active_rot_ );
    watch_cursor_ = ( watch_cursor_ + 1 ) & 3;
    if ( watch_cursor_ == active_idx )
        watch_cursor_ = ( watch_cursor_ + 1 ) & 3;
}

AutoOrient::Result AutoOrient::apply_choice( int best_idx, float best_sc, float second_sc, bool lock_now ) noexcept
{
    Result out {};
    out.prev_rot = active_rot_;
    const int new_rot = auto_orient::idx_to_deg( best_idx );
    active_rot_ = new_rot;
    if ( lock_now )
    {
        phase_ = Phase::Locked;
        locked_ = true;
        burst_left_ = 0;
    }
    out.changed = ( new_rot != out.prev_rot );
    out.locked = locked_;
    out.rot_deg = active_rot_;
    out.best_score = best_sc;
    out.second_score = second_sc;
    return out;
}

AutoOrient::Result AutoOrient::commit_all4( const float scores[4] ) noexcept
{
    Result res { false, locked_, active_rot_, active_rot_, 0.0f, 0.0f };
    if ( !enabled_ || locked_ )
        return res;

    std::array<float, 4> adj { scores[0], scores[1], scores[2], scores[3] };
    if ( exif_rot_ >= 0 )
    {
        const int ei = auto_orient::deg_to_idx( exif_rot_ );
        if ( adj[ei] > 0.0f || !raw_portrait_ )
            adj[ei] += kExifTieBoost;
    }

    int best_idx = 0;
    float best_sc = 0.0f;
    float second_sc = 0.0f;
    find_top_two( adj, best_idx, best_sc, second_sc );

    const bool clear_margin = ( best_sc >= kSignalMin ) &&
                              ( second_sc <= 0.05f || ( best_sc >= second_sc * 1.65f && best_sc - second_sc >= 0.35f ) );
    if ( clear_margin )
    {
        const bool strong = ( best_sc >= kStrongSignal ) && ( second_sc <= 0.08f || best_sc >= second_sc * 2.1f );
        ++votes_[best_idx];
        const bool lock_now = strong || ( votes_[best_idx] >= kVotesToLock );
        if ( !lock_now && burst_left_ > 0 )
            --burst_left_;
        return apply_choice( best_idx, best_sc, second_sc, lock_now );
    }

    if ( burst_left_ > 0 )
        --burst_left_;
    if ( burst_left_ <= 0 )
        phase_ = Phase::Watch;
    res.best_score = best_sc;
    res.second_score = second_sc;
    return res;
}

bool AutoOrient::observe_single( int rot_deg, float score ) noexcept
{
    if ( !enabled_ || locked_ )
        return false;
    if ( phase_ == Phase::Watch )
    {
        if ( rot_deg != active_rot_ )
            advance_watch_cursor();
        if ( score >= kSingleObserveTrigger )
        {
            phase_ = Phase::Probe4;
            burst_left_ = 2;
            return true;
        }
    }
    return false;
}
