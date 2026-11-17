#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "core/callout.hpp"

namespace
{
    constexpr int kMarginPx = 2;
    constexpr int kPadX = 4;
    constexpr int kPadY = 3;
    constexpr int kPillH = GlyphFont::kGlyphH + 2 * kPadY;  // 14 px
    constexpr int kMaxTracks = TrajList::cap;

    struct TrackCand
    {
        int track_idx = 0;
        CalloutRect box {};
    };

    CalloutRect make_even_bounds( const LbGeom& lb ) noexcept
    {
        const int l = lb.left & ~1;
        const int t = lb.top & ~1;
        const int w = lb.w & ~1;
        const int h = lb.h & ~1;
        return { l, t, l + w, t + h };
    }

    bool map_track_box( const TrackState& tr, const CalloutRect& crop, float sx, float sy, CalloutRect& out ) noexcept
    {
        const float x0f = ( tr.cx - tr.bw * 0.5f ) * sx + static_cast<float>( crop.x0 );
        const float y0f = ( tr.cy - tr.bh * 0.5f ) * sy + static_cast<float>( crop.y0 );
        const float x1f = ( tr.cx + tr.bw * 0.5f ) * sx + static_cast<float>( crop.x0 );
        const float y1f = ( tr.cy + tr.bh * 0.5f ) * sy + static_cast<float>( crop.y0 );
        const float cl = static_cast<float>( crop.x0 );
        const float ct = static_cast<float>( crop.y0 );
        const float cr = static_cast<float>( crop.x1 );
        const float cb = static_cast<float>( crop.y1 );
        if ( x1f <= cl || y1f <= ct || x0f >= cr || y0f >= cb )
            return false;
        const int x0 = std::clamp( static_cast<int>( std::floor( x0f ) ), crop.x0, crop.x1 );
        const int y0 = std::clamp( static_cast<int>( std::floor( y0f ) ), crop.y0, crop.y1 );
        const int x1 = std::clamp( static_cast<int>( std::ceil( x1f ) ), crop.x0, crop.x1 );
        const int y1 = std::clamp( static_cast<int>( std::ceil( y1f ) ), crop.y0, crop.y1 );
        if ( x1 - x0 < 2 || y1 - y0 < 2 )
            return false;
        out = { x0, y0, x1, y1 };
        return true;
    }

    int64_t rect_overlap( const CalloutRect& a, const CalloutRect& b ) noexcept
    {
        const int ox = std::min( a.x1, b.x1 ) - std::max( a.x0, b.x0 );
        const int oy = std::min( a.y1, b.y1 ) - std::max( a.y0, b.y0 );
        if ( ox <= 0 || oy <= 0 )
            return 0;
        return static_cast<int64_t>( ox ) * static_cast<int64_t>( oy );
    }

    int64_t score_candidate( const CalloutRect& cand,
                             int self_idx,
                             const CalloutList& placed,
                             const TrackCand* all_boxes,
                             int n_boxes ) noexcept
    {
        const CalloutRect& own = all_boxes[self_idx].box;
        int64_t pill_ov = 0;
        for ( int i = 0; i < placed.n; ++i )
            pill_ov += rect_overlap( cand, placed.d[i].pill );

        int64_t other_ov = 0;
        for ( int i = 0; i < n_boxes; ++i )
        {
            if ( i != self_idx )
                other_ov += rect_overlap( cand, all_boxes[i].box );
        }

        const int64_t own_ov = rect_overlap( cand, own );
        const int ax = ( own.x0 + own.x1 ) / 2;
        const int ay = own.y0;
        const int pcx = ( cand.x0 + cand.x1 ) / 2;
        const int pcy = ( cand.y0 + cand.y1 ) / 2;
        const int64_t dist = static_cast<int64_t>( std::abs( pcx - ax ) ) +
                             static_cast<int64_t>( std::abs( pcy - ay ) );
        return pill_ov * 1000 + other_ov * 100 + own_ov * 10 + dist;
    }

    CalloutRect pick_best_pill( int self_idx,
                                int pw,
                                int ph,
                                const CalloutRect& work,
                                const CalloutList& placed,
                                const TrackCand* all_boxes,
                                int n_boxes ) noexcept
    {
        const CalloutRect& b = all_boxes[self_idx].box;
        const int cx = ( b.x0 + b.x1 - pw ) / 2;
        const int proposals[14][2] = {
            { b.x0, b.y0 - 10 - ph },
            { cx, b.y0 - 10 - ph },
            { b.x1 - pw, b.y0 - 10 - ph },
            { b.x0, b.y0 - 24 - ph },
            { cx, b.y0 - 24 - ph },
            { b.x1 - pw, b.y0 - 24 - ph },
            { b.x1 + 8, b.y0 },
            { b.x0 - 8 - pw, b.y0 },
            { b.x1 + 8, b.y0 + 16 },
            { b.x0 - 8 - pw, b.y0 + 16 },
            { b.x0, b.y1 + 8 },
            { cx, b.y1 + 8 },
            { b.x1 - pw, b.y1 + 8 },
            { b.x0 + 2, b.y0 + 2 },
        };

        const int max_x = work.x1 - pw;
        const int max_y = work.y1 - ph;
        CalloutRect best {};
        int64_t best_score = -1;
        for ( const auto& p : proposals )
        {
            const int x0 = std::clamp( p[0], work.x0, max_x );
            const int y0 = std::clamp( p[1], work.y0, max_y );
            const CalloutRect cand { x0, y0, x0 + pw, y0 + ph };
            const int64_t sc = score_candidate( cand, self_idx, placed, all_boxes, n_boxes );
            if ( best_score < 0 || sc < best_score )
            {
                best_score = sc;
                best = cand;
            }
        }
        return best;
    }

    void push_seg( CalloutItem& item, int x0, int y0, int x1, int y1 ) noexcept
    {
        if ( x1 > x0 && y1 > y0 && item.n_segs < 2 )
            item.segs[item.n_segs++] = { x0, y0, x1, y1 };
    }

    void build_connector( CalloutItem& item ) noexcept
    {
        item.n_segs = 0;
        if ( rect_overlap( item.pill, item.box ) > 0 )
            return;

        const CalloutRect& p = item.pill;
        const CalloutRect& b = item.box;
        const int ox0 = std::max( p.x0, b.x0 );
        const int ox1 = std::min( p.x1, b.x1 );
        if ( ox1 > ox0 )
        {
            const int pcx = ( p.x0 + p.x1 ) / 2;
            const int vx = std::clamp( pcx, ox0, ox1 - 1 );
            if ( p.y1 <= b.y0 )
                push_seg( item, vx, p.y1, vx + 1, b.y0 );
            else if ( b.y1 <= p.y0 )
                push_seg( item, vx, b.y1, vx + 1, p.y0 );
            return;
        }

        const int oy0 = std::max( p.y0, b.y0 );
        const int oy1 = std::min( p.y1, b.y1 );
        if ( oy1 > oy0 )
        {
            const int pcy = ( p.y0 + p.y1 ) / 2;
            const int hy = std::clamp( pcy, oy0, oy1 - 1 );
            if ( p.x1 <= b.x0 )
                push_seg( item, p.x1, hy, b.x0, hy + 1 );
            else if ( b.x1 <= p.x0 )
                push_seg( item, b.x1, hy, p.x0, hy + 1 );
            return;
        }

        // Disjoint in both axes: 2-segment orthogonal L-bend without corner overlap.
        const int lx = std::clamp( ( b.x0 + b.x1 ) / 2, b.x0, b.x1 - 1 );
        const int ly = std::clamp( ( p.y0 + p.y1 ) / 2, p.y0, p.y1 - 1 );
        if ( ly < b.y0 )
            push_seg( item, lx, ly, lx + 1, b.y0 );
        else if ( ly >= b.y1 )
            push_seg( item, lx, b.y1, lx + 1, ly + 1 );

        if ( p.x1 <= lx )
            push_seg( item, p.x1, ly, lx, ly + 1 );
        else if ( p.x0 > lx )
            push_seg( item, lx + 1, ly, p.x0, ly + 1 );
    }

    void assign_accent( int id, CalloutItem& item ) noexcept
    {
        constexpr uint8_t pal[8][3] = {
            { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 }, { 255, 128, 0 },
            { 0, 255, 0 },   { 200, 0, 0 },   { 128, 128, 255 }, { 255, 64, 64 } };
        const int idx = id & 7;
        item.r = pal[idx][0];
        item.g = pal[idx][1];
        item.b = pal[idx][2];
    }

}  // namespace

void CalloutLayout::compute( const TrajList& trajs,
                             const LbGeom& lb,
                             float sx,
                             float sy,
                             const CalloutConfig& cfg,
                             CalloutList& out ) noexcept
{
    out.clear();
    const CalloutRect crop = make_even_bounds( lb );
    const CalloutRect work { crop.x0 + kMarginPx, crop.y0 + kMarginPx, crop.x1 - kMarginPx, crop.y1 - kMarginPx };
    if ( work.x1 - work.x0 <= 0 || work.y1 - work.y0 < kPillH )
        return;

    std::array<TrackCand, kMaxTracks> cands {};
    int nc = 0;
    for ( int k = 0; k < trajs.n && nc < kMaxTracks; ++k )
    {
        const TrackState& tr = trajs.d[k];
        if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
            continue;
        CalloutRect box {};
        if ( !map_track_box( tr, crop, sx, sy, box ) )
            continue;
        cands[nc++] = { k, box };
    }

    for ( int i = 0; i < nc && out.n < CalloutList::cap; ++i )
    {
        const TrackState& tr = trajs.d[cands[i].track_idx];
        const int disp_id = tr.display_id();
        CalloutItem item {};
        item.box = cands[i].box;
        item.text_bytes = GlyphFont::format_person( item.text, GlyphFont::kLabelCap, disp_id, cfg.lang_ru );
        const int ng = GlyphFont::text_len( std::string_view( item.text, static_cast<std::size_t>( item.text_bytes ) ) );
        const int pw = ng * GlyphFont::kGlyphW + 2 * kPadX;
        // Guard before std::clamp: degenerate narrow crops must never trigger lo > hi UB.
        if ( work.x1 - work.x0 < pw || work.y1 - work.y0 < kPillH )
            continue;

        item.pill = pick_best_pill( i, pw, kPillH, work, out, cands.data(), nc );
        item.text_x = item.pill.x0 + kPadX;
        item.text_y = item.pill.y0 + kPadY;
        assign_accent( disp_id, item );
        build_connector( item );
        out.push( item );
    }
}
