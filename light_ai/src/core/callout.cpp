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
    constexpr int kPillH = glyph_font::kGlyphH + 2 * kPadY;  // 14 px
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

    struct CandEval
    {
        CalloutRect rect {};
        int64_t ov_cost = 0;
        int clamp_shift = 0;
        int64_t total = 0;
        int idx = 0;
    };

    int find_prev_slot( const CalloutHistory& hist, int disp_id ) noexcept
    {
        for ( int i = 0; i < hist.entries.n; ++i )
        {
            if ( hist.entries.d[i].id == disp_id )
                return hist.entries.d[i].slot;
        }
        return -1;
    }

    CandEval eval_candidate( int idx,
                             const int prop[2],
                             int pw,
                             int ph,
                             const CalloutRect& work,
                             int self_idx,
                             const CalloutList& placed,
                             const TrackCand* all_boxes,
                             int n_boxes ) noexcept
    {
        const CalloutRect& own = all_boxes[self_idx].box;
        const int x0 = std::clamp( prop[0], work.x0, work.x1 - pw );
        const int y0 = std::clamp( prop[1], work.y0, work.y1 - ph );
        const CalloutRect cand { x0, y0, x0 + pw, y0 + ph };

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
        const int64_t ov_cost = pill_ov * 10000 + other_ov * 1000 + own_ov * 100;
        const int clamp_shift = std::abs( x0 - prop[0] ) + std::abs( y0 - prop[1] );
        // Use doubled integer coordinates so integer /2 rounding never breaks symmetry between left and right.
        const int64_t dx2 = std::abs( ( cand.x0 + cand.x1 ) - ( own.x0 + own.x1 ) );
        const int64_t dy2 = std::abs( ( cand.y0 + cand.y1 ) - 2 * own.y0 );
        const int64_t total = ov_cost + static_cast<int64_t>( clamp_shift ) * 8 + ( dx2 + dy2 ) / 2;
        return CandEval { cand, ov_cost, clamp_shift, total, idx };
    }

    CandEval pick_best_pill( int self_idx,
                             int prev_slot,
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

        CandEval best {};
        best.total = -1;
        CandEval prev_eval {};
        for ( int idx = 0; idx < 14; ++idx )
        {
            const CandEval cur = eval_candidate( idx, proposals[idx], pw, ph, work, self_idx, placed, all_boxes, n_boxes );
            if ( idx == prev_slot )
                prev_eval = cur;
            if ( best.total < 0 || cur.total < best.total )
                best = cur;
        }

        if ( prev_slot >= 0 && prev_slot < 14 && best.idx != prev_slot )
        {
            const bool prev_clean = ( prev_eval.ov_cost == 0 && prev_eval.clamp_shift <= 4 );
            const bool promote_top = ( prev_slot >= 6 && best.idx <= 2 && best.ov_cost == 0 &&
                                       best.clamp_shift == 0 && b.y0 - 14 - ph >= work.y0 );
            if ( prev_clean && !promote_top )
                return prev_eval;
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

    int collect_track_cands( const TrajList& trajs,
                             const CalloutRect& crop,
                             float sx,
                             float sy,
                             std::array<TrackCand, kMaxTracks>& cands ) noexcept
    {
        int nc = 0;
        for ( int k = 0; k < trajs.n && nc < kMaxTracks; ++k )
        {
            const TrackState& tr = trajs.d[k];
            if ( tr.state != TRACK_TENT && tr.state != TRACK_CONF )
                continue;
            CalloutRect box {};
            if ( map_track_box( tr, crop, sx, sy, box ) )
                cands[nc++] = { k, box };
        }
        return nc;
    }

    void append_callout_for_track( int i,
                                   int disp_id,
                                   const CalloutConfig& cfg,
                                   const CalloutRect& work,
                                   const CalloutHistory& hist,
                                   const TrackCand* cands,
                                   int nc,
                                   CalloutList& out,
                                   CalloutHistory& next_hist ) noexcept
    {
        CalloutItem item {};
        item.box = cands[i].box;
        item.text_bytes = glyph_font::format_person( item.text, glyph_font::kLabelCap, disp_id, cfg.lang_ru );
        const int ng = glyph_font::text_len( std::string_view( item.text, static_cast<std::size_t>( item.text_bytes ) ) );
        const int pw = ng * glyph_font::kGlyphW + 2 * kPadX;
        // Guard before std::clamp: degenerate narrow crops must never trigger lo > hi UB.
        if ( work.x1 - work.x0 < pw || work.y1 - work.y0 < kPillH )
            return;
        const CandEval best = pick_best_pill( i, find_prev_slot( hist, disp_id ), pw, kPillH, work, out, cands, nc );
        item.pill = best.rect;
        item.text_x = item.pill.x0 + kPadX;
        item.text_y = item.pill.y0 + kPadY;
        assign_accent( disp_id, item );
        build_connector( item );
        out.push( item );
        next_hist.entries.push( { disp_id, best.idx } );
    }

}  // namespace

void compute_callouts( const TrajList& trajs,
                       const LbGeom& lb,
                       float sx,
                       float sy,
                       const CalloutConfig& cfg,
                       CalloutHistory& hist,
                       CalloutList& out ) noexcept
{
    out.clear();
    const CalloutRect crop = make_even_bounds( lb );
    const CalloutRect work { crop.x0 + kMarginPx, crop.y0 + kMarginPx, crop.x1 - kMarginPx, crop.y1 - kMarginPx };
    if ( work.x1 - work.x0 <= 0 || work.y1 - work.y0 < kPillH )
    {
        hist.entries.clear();
        return;
    }

    std::array<TrackCand, kMaxTracks> cands {};
    const int nc = collect_track_cands( trajs, crop, sx, sy, cands );
    CalloutHistory next_hist {};
    for ( int i = 0; i < nc && out.n < CalloutList::cap; ++i )
        append_callout_for_track( i, trajs.d[cands[i].track_idx].display_id(), cfg, work, hist, cands.data(), nc, out, next_hist );
    hist = next_hist;
}

void compute_callouts( const TrajList& trajs,
                       const LbGeom& lb,
                       float sx,
                       float sy,
                       const CalloutConfig& cfg,
                       CalloutList& out ) noexcept
{
    CalloutHistory tmp {};
    compute_callouts( trajs, lb, sx, sy, cfg, tmp, out );
}
