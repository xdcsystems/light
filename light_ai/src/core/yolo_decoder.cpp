#include <algorithm>
#include <cmath>

#include "core/yolo_decoder.hpp"
#include "core/box_math.hpp"
#include "core/fp16.hpp"

namespace
{
    // The anchor set of a head from its stride: P3 / P4 / P5, -1 = unsupported.
    int stride_to_head( int stride )
    {
        if ( stride == 8 )
            return 0;
        if ( stride == 16 )
            return 1;
        if ( stride == 32 )
            return 2;
        return -1;
    }

    // yolov5 default anchors, already in model-input pixels per stride.
    constexpr float kANCH[3][3][2] = {
            { { 10, 13 }, { 16, 30 }, { 33, 23 } },
            { { 30, 61 }, { 62, 45 }, { 59, 119 } },
            { { 116, 90 }, { 156, 198 }, { 373, 326 } },
    };

    // Accepts detections whose center sits slightly past the letterbox boundary
    // (half-visible people exiting/entering frame edges) as long as the box overlaps
    // the visible band by >= 2 model pixels per axis.
    bool box_overlaps_band( float cx, float cy, float bw_px, float bh_px, int stride,
                            float left, float top, float bw, float bh ) noexcept
    {
        const float pad_x = std::min( 16.0f, std::max( static_cast<float>( stride ) * 0.5f, bw_px * 0.25f ) );
        const float pad_y = std::min( 16.0f, std::max( static_cast<float>( stride ) * 0.5f, bh_px * 0.25f ) );
        if ( cy < top - pad_y || cy >= top + bh + pad_y || cx < left - pad_x || cx >= left + bw + pad_x )
            return false;
        const float vx = std::min( cx + bw_px * 0.5f, left + bw ) - std::max( cx - bw_px * 0.5f, left );
        const float vy = std::min( cy + bh_px * 0.5f, top + bh ) - std::max( cy - bh_px * 0.5f, top );
        return vx >= 2.0f && vy >= 2.0f;
    }
}  // namespace

YoloDecoder::YoloDecoder( float conf_thr, float iou_thr, const Log& log )
  : conf_thr_( conf_thr ), iou_thr_( iou_thr ), log_( log )
{
}

void YoloDecoder::decode( const YoloHead* heads, int n_heads, const YoloFrame& f, DetList& out )
{
    // Fresh frame: out is a preallocated pipeline member (DetList), it
    // is never reconstructed between calls -- clear it at entry.
    // Without this, detections from previous frames accumulate until the 128
    // cap saturates and the visible box set freezes
    out.clear();
    if ( !band_fits( f ) )
        return;

    const Band b { static_cast<float>( f.lb.left ), static_cast<float>( f.lb.top ), static_cast<float>( f.lb.w ),
                   static_cast<float>( f.lb.h ) };
    nc_ = 0;
    dropped_ = 0;
    const int mw = static_cast<int>( f.model_w );
    const int mh = static_cast<int>( f.model_h );
    for ( int gi = 0; gi < n_heads && gi < kMaxHeads; ++gi )
    {
        const YoloHead& hd = heads[gi];
        if ( !hd.data )
            continue;
        int stride = 0;
        const int set = anchor_set( hd, gi, mw, mh, stride );
        if ( set < 0 )
            continue;
        collect_head( HeadScan { hd.data, static_cast<size_t>( hd.h ) * hd.w, hd.h, hd.w, hd.ch / 3, stride, set }, b );
    }

    if ( dropped_ > 0 )
        log_.warn() << "yolo: candidate cap " << kMaxCand << " reached, " << dropped_ << " candidates dropped (raise --conf)\n";

    suppress();
    const int dropped_out = emit( f, b, out );
    if ( dropped_out > 0 )
        log_.warn() << "yolo: det cap " << DetList::cap << " reached, " << dropped_out << " detections dropped\n";
}

// Letterbox geometry must fit the model input: the band is rows
// [lb_top, lb_top + lb_h) of the model frame. A violation here would
// silently misplace every detection, so it is a hard error.
bool YoloDecoder::band_fits( const YoloFrame& f ) const noexcept
{
    const float lb_left = static_cast<float>( f.lb.left );
    const float lb_w = static_cast<float>( f.lb.w );
    const float lb_top = static_cast<float>( f.lb.top );
    const float lb_h = static_cast<float>( f.lb.h );
    if ( lb_w > 0.0f && lb_h > 0.0f && lb_top >= 0.0f && lb_top + lb_h <= f.model_h && lb_left >= 0.0f && lb_left + lb_w <= f.model_w )
        return true;
    log_.warn() << "yolo_postprocess: letterbox band x=[" << lb_left << ", " << lb_left + lb_w << ") y=[" << lb_top << ", "
                << lb_top + lb_h << ") does not fit model " << f.model_w << 'x' << f.model_h << '\n';
    return false;
}

// The shape check of one head: the layout must be a 3-anchor yolov5 head --
// the width divides the model input, the stride is 8/16/32 (the head index is
// not trusted: RKNN does not promise the P3/P4/P5 order, and 8 << gi silently
// shifts every box when the heads come out in another order), the height
// matches the model at that stride and ch is a 3-anchor layout (ch % 3 == 0;
// the strict ch == 255 of the standard 80-class model is the startup check's
// job). One warn per failure; the caller decides fatal (the startup check)
// vs skip (decode).
bool yolo_head_shape( const YoloHead& hd, int gi, int mw, int mh, const Log& log, int& stride, int& head )
{
    stride = 0;
    head = -1;
    if ( hd.w <= 0 || mw % hd.w != 0 )
    {
        log.warn() << "yolo: head " << gi << " width " << hd.w << " does not divide model " << mw << '\n';
        return false;
    }
    stride = mw / hd.w;
    head = stride_to_head( stride );
    if ( head < 0 )
    {
        log.warn() << "yolo: head " << gi << " unsupported stride " << stride << '\n';
        return false;
    }
    if ( hd.h <= 0 || hd.h != mh / stride )
    {
        log.warn() << "yolo: head " << gi << " height " << hd.h << " does not match model " << mh << " at stride "
                   << stride << '\n';
        return false;
    }
    // At least one class channel per anchor (4 box + 1 obj + 1 class = 6,
    // ch >= 18): below that best_class() would read past the head (M-14).
    const int per = hd.ch / 3;  // 85 = 4+1+80
    if ( hd.ch < 18 || per * 3 != hd.ch )
    {
        log.warn() << "yolo: unexpected head layout (per=" << per << ", ch=" << hd.ch << ")\n";
        return false;  // not a 3-anchor yolov5 head - skip
    }
    return true;
}

// The anchor set of head gi, or -1 (logged): the shape check is shared with
// the startup check (yolo_head_shape), the warn text comes from there.
int YoloDecoder::anchor_set( const YoloHead& hd, int gi, int mw, int mh, int& stride ) const
{
    int head = -1;
    return yolo_head_shape( hd, gi, mw, mh, log_, stride, head ) ? head : -1;
}

// Stage 1/2: decode one head into candidate boxes. The tensor is
// channel-major fp16: element (c, gy, gx) lives at c*h*w + gy*w + gx,
// the anchor slot is k*per*cell_hw further.
void YoloDecoder::collect_head( const HeadScan& s, const Band& b ) noexcept
{
    for ( int gy = 0; gy < s.h; ++gy )
    {
        for ( int gx = 0; gx < s.w; ++gx )
        {
            const size_t base = ( static_cast<size_t>( gy ) * s.w + gx );
            for ( int k = 0; k < 3; ++k )
            {
                const size_t off = base + static_cast<size_t>( k ) * s.per * s.cell_hw;
                Box box {};
                if ( decode_anchor( s, off, k, gx, gy, b, box ) )
                    push_candidate( box );
            }
        }
    }
}

// One anchor of one cell -> a candidate, or false. The obj pre-check stays
// before the class scan - that order is the confidence shortcut.
bool YoloDecoder::decode_anchor( const HeadScan& s, size_t off, int k, int gx, int gy, const Band& b, Box& out ) const noexcept
{
    const size_t cell_hw = s.cell_hw;
    const float obj = fp16_to_float( s.data[off + 4 * cell_hw] );
    // Post-sigmoid scores are in (0, 1], so conf = obj * cls
    // cannot exceed obj. Anchors below conf_thr are dead.
    // The old 0.05 cut still walked every class channel
    // whenever the caller asked for a higher threshold.
    if ( !std::isfinite( obj ) || !( obj >= conf_thr_ ) )
        return false;

    int best = 0;
    const float best_cls = best_class( s, off, best );
    // Class scan gate: best_cls <= 0 means no class channel
    // beat zero (NaN never passes '>'), the pre-scan best
    // would map to an invalid class id. No evidence -> no
    // candidate (also kills conf==0 phantoms at thr <= 0).
    if ( !std::isfinite( best_cls ) || best_cls <= 0.0f )
        return false;
    const float conf = obj * best_cls;
    if ( !std::isfinite( conf ) || !( conf >= conf_thr_ ) )
        return false;

    const float* an = kANCH[s.anchors][k];
    const int stride = s.stride;
    const float cx = ( fp16_to_float( s.data[off + 0 * cell_hw] ) * 2.0f - 0.5f + gx ) * stride;
    const float cy = ( fp16_to_float( s.data[off + 1 * cell_hw] ) * 2.0f - 0.5f + gy ) * stride;
    const float bw = fp16_to_float( s.data[off + 2 * cell_hw] );
    const float bh = fp16_to_float( s.data[off + 3 * cell_hw] );
    const float bw_px = ( bw * 2.0f ) * ( bw * 2.0f ) * an[0];
    const float bh_px = ( bh * 2.0f ) * ( bh * 2.0f ) * an[1];

    // Reject NaN and +/-Inf so non-finite boxes cannot poison NMS IoU or emit().
    if ( !std::isfinite( cx ) || !std::isfinite( cy ) || !std::isfinite( bw_px ) || !std::isfinite( bh_px ) )
        return false;
    if ( !( bw_px > 0.0f ) || !( bh_px > 0.0f ) )
        return false;  // an empty box carries no geometry (emit would drop it anyway)

    if ( !box_overlaps_band( cx, cy, bw_px, bh_px, stride, b.left, b.top, b.w, b.h ) )
        return false;  // bar

    out = { cx - bw_px * 0.5f, cy - bh_px * 0.5f, cx + bw_px * 0.5f, cy + bh_px * 0.5f, conf, best - 5, 0 };
    return true;
}

// The best class channel of one anchor: its score, the channel index in best
// (the class id is best - 5). 0 when no channel beats zero.
float YoloDecoder::best_class( const HeadScan& s, size_t off, int& best ) const noexcept
{
    float best_cls = 0.0f;
    for ( int c = 5; c < s.per; ++c )
    {
        // A non-finite channel never wins: +Inf used to take the anchor and
        // then drop it whole, losing the finite best class.
        if ( const float v = fp16_to_float( s.data[off + c * s.cell_hw] ); std::isfinite( v ) && v > best_cls )
        {
            best_cls = v;
            best = c;
        }
    }
    return best_cls;
}

void YoloDecoder::push_candidate( const Box& box ) noexcept
{
    if ( nc_ < kMaxCand )
    {
        Box& dst = cand_[nc_];
        dst = box;
        dst.seq = nc_;
        ++nc_;
    }
    else
    {
        ++dropped_;  // cap hit: reported once per frame in decode()
    }
}

// Stage 2/2, part 1: class-aware greedy NMS over the candidates (standard
// yolov5 behaviour: boxes of different classes never suppress each other).
void YoloDecoder::suppress() noexcept
{
    std::sort( cand_.begin(), cand_.begin() + nc_, []( const Box& a, const Box& b ) {
        return a.conf != b.conf ? a.conf > b.conf : a.seq < b.seq;
    } );
    std::fill_n( sup_.begin(), nc_, uint8_t { 0 } );
    for ( int i = 0; i < nc_; ++i )
    {
        if ( sup_[i] )
            continue;
        const Box& a = cand_[i];
        for ( int j = i + 1; j < nc_; ++j )
        {
            const Box& c = cand_[j];
            // Corner-based NMS candidates: the shared corner IoU (box_math.hpp),
            // the +1e-6f degenerate-union guard lives there.
            if ( !sup_[j] && a.cls == c.cls && box_iou_corners( a.x1, a.y1, a.x2, a.y2, c.x1, c.y1, c.x2, c.y2 ) > iou_thr_ )
                sup_[j] = 1;
        }
    }
}

// Stage 2/2, part 2: the survivors, in confidence order, back to camera
// pixels and into the preallocated detector list.
int YoloDecoder::emit( const YoloFrame& f, const Band& b, DetList& out ) const noexcept
{
    // Axial mapping: scene = (det - band origin) * cam / band size.
    // Landscape: the band fills the tensor width (lb_left = 0, lb_w = model_w)
    // - identical to the legacy uniform-by-width mapping.
    // Portrait: the band fills the tensor height; the axes scale
    // separately (both are cam/lb for a uniform fit).
    const float sx = f.cam_w / b.w;
    const float sy = f.cam_h / b.h;
    int dropped = 0;
    for ( int i = 0; i < nc_; ++i )
    {
        if ( sup_[i] )
            continue;
        const Box& c = cand_[i];
        // Band-crop before scale: clamping d.x alone pinned the left
        // edge to 0 while d.w kept the FULL width -- the right edge
        // shifted right by the clipped amount and the box stretched
        // into the scene, corrupting the IoU association downstream.
        // The cam_w/cam_h clamps stay: with d.x >= 0 they clip the
        // right (bottom) edge to the band boundary exactly as
        // clamping x2 (y2) to lb_left + lb_w (lb_top + lb_h) would.
        const float x1_c = std::max( c.x1, b.left );
        const float y1_c = std::max( c.y1, b.top );
        Detection d {};
        d.x = ( x1_c - b.left ) * sx;
        d.y = ( y1_c - b.top ) * sy;
        d.w = std::max( 0.0f, std::min( f.cam_w - d.x, ( c.x2 - x1_c ) * sx ) );
        d.h = std::max( 0.0f, std::min( f.cam_h - d.y, ( c.y2 - y1_c ) * sy ) );
        d.cls = c.cls;
        d.conf = c.conf;
        // Degenerate-box guard: a candidate whose x1 exceeded model_w
        // clamps d.x to cam_w, leaving d.w <= 0 (same for h). Such a box
        // would ride the smoother and the drawing paths with broken
        // geometry. Skip it before any consumer sees it.
        if ( d.w <= 0.0f || d.h <= 0.0f )
            continue;
        if ( !out.push( d ) )
            ++dropped;
    }
    return dropped;
}
