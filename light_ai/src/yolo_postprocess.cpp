#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "yolo_postprocess.hpp"
#include "rga_resizer.h"
#include "rknn_engine.hpp"
#include "box_math.hpp"

namespace
{
    // fp16 (IEEE 754 half) -> float via ARM64 hardware __fp16 (single fcvt).
    inline float fp16_to_float( uint16_t h )
    {
#if defined( __aarch64__ )
        __fp16 f16 = 0;
        std::memcpy( &f16, &h, sizeof( f16 ) );
        return static_cast<float>( f16 );
#else
        const uint32_t sign = static_cast<uint32_t>( h & 0x8000U ) << 16U;
        const uint32_t exp = ( h >> 10U ) & 0x1FU;
        const uint32_t mant = h & 0x03FFU;
        uint32_t bits = 0;
        if ( exp == 0 )
            bits = mant ? ( sign | ( ( 113U ) << 23U ) | ( mant << 13U ) ) : sign;
        else if ( exp == 31 )
            bits = sign | 0x7F800000U | ( mant << 13U );
        else
            bits = sign | ( ( exp + 112U ) << 23U ) | ( mant << 13U );
        float out = 0.0f;
        std::memcpy( &out, &bits, sizeof( out ) );
        return out;
#endif
    }

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

    struct Box
    {
        float x1, y1, x2, y2;  // det coordinates (model input)
        float conf;
        int cls;
    };

    inline float iou( const Box& a, const Box& b )
    {
        // Corner-based NMS candidates: the shared corner IoU (box_math.hpp),
        // the +1e-6f degenerate-union guard moved there.
        return box_iou_corners( a.x1, a.y1, a.x2, a.y2, b.x1, b.y1, b.x2, b.y2 );
    }

    // Candidate buffer on the stack: fixed capacity, no heap. The early
    // cut (obj < conf_thr) keeps the real flow to single digits; the cap
    // is a safety net: an overflow is counted and reported once per frame
    // in yolo_postprocess(), never silent.
    constexpr int kMaxCand = 4096;

    // Stage 2/2 of yolo_postprocess(): class-aware greedy NMS over the
    // candidates (standard yolov5 behaviour: boxes of different classes
    // never suppress each other), then the coordinate transform back to
    // camera pixels and emission into the preallocated detector list.
    void emit_nms( std::array<Box, kMaxCand>& cand, int nc, float iou_thr, float sx, float sy, float lb_left, float lb_top, float cam_w, float cam_h,
            DetList& out )
    {
        std::sort( cand.begin(), cand.begin() + nc, []( const Box& a, const Box& b ) {
            return a.conf > b.conf;
        } );
        std::array<uint8_t, kMaxCand> sup {};
        for ( int i = 0; i < nc; ++i )
        {
            if ( sup[i] )
                continue;
            for ( int j = i + 1; j < nc; ++j )
            {
                if ( !sup[j] && cand[i].cls == cand[j].cls && iou( cand[i], cand[j] ) > iou_thr )
                    sup[j] = true;
            }
            const Box& b = cand[i];
            // Band-crop before scale: clamping d.x alone pinned the left
            // edge to 0 while d.w kept the FULL width -- the right edge
            // shifted right by the clipped amount and the box stretched
            // into the scene, corrupting the IoU association downstream.
            // The cam_w/cam_h clamps stay: with d.x >= 0 they clip the
            // right (bottom) edge to the band boundary exactly as
            // clamping x2 (y2) to lb_left + lb_w (lb_top + lb_h) would.
            const float x1_c = std::max( b.x1, lb_left );
            const float y1_c = std::max( b.y1, lb_top );
            Detection d{};
            d.x = ( x1_c - lb_left ) * sx;
            d.y = ( y1_c - lb_top ) * sy;
            d.w = std::max( 0.0f, std::min( cam_w - d.x, ( b.x2 - x1_c ) * sx ) );
            d.h = std::max( 0.0f, std::min( cam_h - d.y, ( b.y2 - y1_c ) * sy ) );
            d.cls = b.cls;
            d.conf = b.conf;
            // Degenerate-box guard: a candidate whose x1 exceeded model_w
            // clamps d.x to cam_w, leaving d.w <= 0 (same for h). Such a box
            // would ride the smoother and the drawing paths with broken
            // geometry. Skip it before any consumer sees it.
            if ( d.w <= 0.0f || d.h <= 0.0f )
                continue;
            out.push( d );
        }
    }

    // Stage 1/2 of yolo_postprocess(): decode one yolov5 head into
    // candidate boxes. The tensor is channel-major fp16: element
    // (c, gy, gx) lives at c*h*w + gy*w + gx, the anchor slot is
    // k*per*cell_hw further. The obj pre-check stays before the class
    // scan - that order is the confidence shortcut.
    void collect_head( int gi, const uint16_t* data, int h, int w, int per, int stride, float lb_left, float lb_top, float lb_w, float lb_h, float conf_thr,
            std::array<Box, kMaxCand>& cand, int& nc, int& dropped )
    {
        for ( int gy = 0; gy < h; ++gy )
        {
            for ( int gx = 0; gx < w; ++gx )
            {
                const size_t base = ( static_cast<size_t>( gy ) * w + gx );
                // channel-major: element (c, gy, gx) is at c*h*w + base
                const size_t cell_hw = static_cast<size_t>( h ) * w;
                // anchor loop is merged with the confidence shortcut below
                for ( int k = 0; k < 3; ++k )
                {
                    const size_t off = base + static_cast<size_t>( k ) * per * cell_hw;
                    const float obj = fp16_to_float( data[off + 4 * cell_hw] );
                    // Post-sigmoid scores are in (0, 1], so conf = obj * cls
                    // cannot exceed obj. Anchors below conf_thr are dead.
                    // The old 0.05 cut still walked every class channel
                    // whenever the caller asked for a higher threshold.
                    if ( !( obj >= conf_thr ) )
                        continue;  // NaN-safe: NaN fails the comparison

                    int best = 0;
                    float best_cls = 0.0f;
                    for ( int c = 5; c < per; ++c )
                    {
                        const float v = fp16_to_float( data[off + c * cell_hw] );
                        if ( v > best_cls )
                        {
                            best_cls = v;
                            best = c;
                        }
                    }
                    // Class scan gate: best_cls <= 0 means no class channel
                    // beat zero (NaN never passes '>'), the pre-scan best
                    // would map to an invalid class id. No evidence -> no
                    // candidate (also kills conf==0 phantoms at thr <= 0).
                    if ( best_cls <= 0.0f )
                        continue;
                    const float conf = obj * best_cls;
                    if ( !( conf >= conf_thr ) )  // NaN-safe: NaN passes a plain <
                        continue;

                    const float* an = kANCH[gi][k];
                    const float cx = ( fp16_to_float( data[off + 0 * cell_hw] ) * 2.0f - 0.5f + gx ) * stride;
                    const float cy = ( fp16_to_float( data[off + 1 * cell_hw] ) * 2.0f - 0.5f + gy ) * stride;
                    const float bw = fp16_to_float( data[off + 2 * cell_hw] );
                    const float bh = fp16_to_float( data[off + 3 * cell_hw] );
                    const float bw_px = ( bw * 2.0f ) * ( bw * 2.0f ) * an[0];
                    const float bh_px = ( bh * 2.0f ) * ( bh * 2.0f ) * an[1];

                    // NaN guard: NaN in any coordinate would sail through
                    // every plain < / || filter below and land in the box list.
                    if ( cx != cx || cy != cy || bw_px != bw_px || bh_px != bh_px )
                        continue;

                    if ( cy < lb_top || cy >= lb_top + lb_h || cx < lb_left || cx >= lb_left + lb_w )
                        continue;  // bar

                    if ( nc < kMaxCand )
                        cand[nc++] = { cx - bw_px / 2, cy - bh_px / 2, cx + bw_px / 2, cy + bh_px / 2, conf, best - 5 };
                    else
                        ++dropped;  // cap hit: reported once per frame in yolo_postprocess()
                }
            }
        }
    }

}  // namespace

void yolo_postprocess( const RknnEngine& npu, float cam_w, float cam_h, float model_w, float model_h, const LbGeom& lb,
        DetList& out, float conf_thr, float iou_thr )
{
    const float lb_left = static_cast<float>( lb.left );
    const float lb_w = static_cast<float>( lb.w );
    const float lb_top = static_cast<float>( lb.top );
    const float lb_h = static_cast<float>( lb.h );
    // Letterbox geometry must fit the model input: the band is rows
    // [lb_top, lb_top + lb_h) of the model frame. A violation here would
    // silently misplace every detection, so it is a hard error.
    if ( lb_w <= 0.0f || lb_h <= 0.0f || lb_top < 0.0f || lb_top + lb_h > model_h || lb_left < 0.0f || lb_left + lb_w > model_w )
    {
        std::cerr << "yolo_postprocess: letterbox band x=[" << lb_left << ", " << lb_left + lb_w << ") y=[" << lb_top << ", "
                  << lb_top + lb_h << ") does not fit model " << model_w << 'x' << model_h << '\n';
        out.clear();
        return;
    }

    // Fresh frame: out is a preallocated pipeline member (DetList), it
    // is never reconstructed between calls — clear it at entry.
    // Without this, detections from previous frames accumulate until the 128
    // cap saturates and the visible box set freezes
    out.clear();

    std::array<Box, kMaxCand> cand{};
    int nc = 0;
    int dropped = 0;

    // Axial mapping: scene = (det - band origin) * cam / band size.
    // Landscape: the band fills the tensor width (lb_left = 0, lb_w = model_w)
    // - identical to the legacy uniform-by-width mapping.
    // Portrait: the band fills the tensor height; the axes scale
    // separately (both are cam/lb for a uniform fit).
    const float sx = cam_w / lb_w;
    const float sy = cam_h / lb_h;

    for ( size_t gi = 0; gi < npu.output_attrs().size() && gi < 3; ++gi )
    {
        const auto& attr = npu.output_attrs()[gi];
        const uint16_t* data = reinterpret_cast<const uint16_t*>( npu.output_data( gi ) );
        if ( !data || attr.n_dims != 4 )
            continue;

        // Stride from the map size, not from the output index. RKNN does
        // not promise P3/P4/P5 order; 8<<gi silently shifts every box
        // when the heads come out in another order.
        // NCHW: dims = {1, 255, h, w}
        const int h = static_cast<int>( attr.dims[2] );
        const int w = static_cast<int>( attr.dims[3] );
        const int mw = static_cast<int>( model_w );
        if ( w <= 0 || mw % w != 0 )
        {
            std::cerr << "yolo: head " << gi << " width " << w
                      << " does not divide model " << mw << '\n';
            continue;
        }
        const int stride = mw / w;
        const int head = stride_to_head( stride );
        if ( head < 0 )
        {
            std::cerr << "yolo: head " << gi << " unsupported stride " << stride << '\n';
            continue;
        }
        const int per = static_cast<int>( attr.dims[1] ) / 3;  // 85 = 4+1+80
        if ( per * 3 != static_cast<int>( attr.dims[1] ) )
        {
            std::cerr << "yolo: unexpected head layout (per="
                      << per << ", ch=" << attr.dims[1]
                      << ")\n";
            continue;  // not a 3-anchor yolov5 head - skip
        }

        collect_head( head, data, h, w, per, stride, lb_left, lb_top, lb_w, lb_h, conf_thr, cand, nc, dropped );
    }

    if ( dropped > 0 )
        std::cerr << "yolo: candidate cap " << kMaxCand << " reached, " << dropped
                  << " candidates dropped (raise --conf)\n";

    emit_nms( cand, nc, iou_thr, sx, sy, lb_left, lb_top, cam_w, cam_h, out );
}
