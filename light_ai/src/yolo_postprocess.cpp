#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "yolo_postprocess.hpp"
#include "rknn_engine.hpp"

namespace
{
    // fp16 (IEEE 754 half) -> float, bit-exact, no arch extensions needed.
    inline float fp16_to_float( uint16_t h )
    {
        const uint32_t sign = ( h >> 15 ) & 1u;
        const uint32_t exp = ( h >> 10 ) & 31u;
        const uint32_t man = h & 0x3FFu;

        // fp32 assembly from half fields: the low 13 mantissa bits are
        // always zero, so the shift is lossless.
        const auto bits = []( uint32_t s, uint32_t e, uint32_t m ) {
            return ( s << 31 ) | ( e << 23 ) | ( m << 13 );
        };
        const auto to_float = []( uint32_t b ) {
            float f = 0.0f;
            std::memcpy( &f, &b, 4 );
            return f;
        };

        if ( exp == 0 && man == 0 )
            return to_float( bits( sign, 0, 0 ) );  // +-0
        if ( exp == 31 )
            return to_float( bits( sign, 0xFF, man ) );  // +-inf / NaN
        if ( exp != 0 )
            return to_float( bits( sign, exp - 15 + 127, man ) );  // normal

        // Subnormal without the shift loop: 2^-14 * man/1024 equals
        // 2^-14*(1 + man/1024) - 2^-14, and that subtraction is exact in
        // fp32 (no underflow below 2^-14), so the result is bit-identical
        // to the loop normalization.
        return std::copysign( to_float( bits( 0, 127 - 15 + 1, man ) ) - 0x1.0p-14f,
                sign ? -1.0f : 1.0f );
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
        const float ix1 = std::max( a.x1, b.x1 );
        const float iy1 = std::max( a.y1, b.y1 );
        const float ix2 = std::min( a.x2, b.x2 );
        const float iy2 = std::min( a.y2, b.y2 );
        const float iw = std::max( 0.0f, ix2 - ix1 );
        const float ih = std::max( 0.0f, iy2 - iy1 );
        const float inter = iw * ih;
        const float area_a = ( a.x2 - a.x1 ) * ( a.y2 - a.y1 );
        const float area_b = ( b.x2 - b.x1 ) * ( b.y2 - b.y1 );
        // +1e-6f: both degenerate zero-area boxes must not crash the NMS.
        return inter / ( area_a + area_b - inter + 1e-6f );
    }

}  // namespace

void yolo_postprocess( const RknnEngine& npu, float cam_w, float cam_h, float model_w, float model_h, float lb_top, float lb_h,
        DetList& out, float conf_thr, float iou_thr )
{
    // Letterbox geometry must fit the model input: the band is rows
    // [lb_top, lb_top + lb_h) of the model frame. A violation here would
    // silently misplace every detection, so it is a hard error.
    if ( lb_top < 0.0f || lb_top + lb_h > model_h )
    {
        std::cerr << "yolo_postprocess: letterbox band [" << lb_top << ", " << lb_top + lb_h << ") does not fit model height " << model_h
                  << '\n';
        out.clear();
        return;
    }

    // Fresh frame: out is a preallocated pipeline member (DetList), it
    // is never reconstructed between calls — clear it at entry. Without
    // this, detections from previous frames accumulate until the 128
    // cap saturates and the visible box set freezes (regression from
    // 7c99906, caught live on 2026-10-27).
    out.clear();

    // Candidate buffer on the stack: fixed capacity, no heap. The early
    // cut (obj < 0.05f) keeps the real flow to single digits; the cap
    // is a safety net.
    constexpr int k_max_cand = 1024;
    std::array<Box, k_max_cand> cand{};
    int nc = 0;

    const float scale = cam_w / model_w;  // uniform, by width

    for ( size_t gi = 0; gi < npu.output_attrs().size() && gi < 3; ++gi )
    {
        const auto& attr = npu.output_attrs()[gi];
        const uint16_t* data = reinterpret_cast<const uint16_t*>( npu.output_data( gi ) );
        if ( !data || attr.n_dims != 4 )
            continue;

        // NCHW: dims = {1, 255, h, w}
        const int h = static_cast<int>( attr.dims[2] );
        const int w = static_cast<int>( attr.dims[3] );
        const int stride = 8 << gi;
        const int per = static_cast<int>( attr.dims[1] ) / 3;  // 85 = 4+1+80
        if ( per * 3 != static_cast<int>( attr.dims[1] ) )
        {
            std::cerr << "yolo: unexpected head layout (per="
                      << per << ", ch=" << attr.dims[1]
                      << ")\n";
            continue;  // not a 3-anchor yolov5 head - skip
        }

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
                    if ( !( obj >= 0.05f ) )
                        continue;  // cheap early cut, below any usable conf_thr (NaN-safe)

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
                    const float conf = obj * best_cls;
                    if ( !( conf >= conf_thr ) )  // NaN-safe: NaN passes a plain <
                        continue;

                    const float* an = kANCH[gi][k];
                    const float cx = ( fp16_to_float( data[off + 0 * cell_hw] ) * 2.0f - 0.5f + gx ) * stride;
                    const float cy = ( fp16_to_float( data[off + 1 * cell_hw] ) * 2.0f - 0.5f + gy ) * stride;
                    const float bw = fp16_to_float( data[off + 2 * cell_hw] );
                    const float bh = fp16_to_float( data[off + 3 * cell_hw] );
                    const float w = ( bw * 2.0f ) * ( bw * 2.0f ) * an[0];
                    const float h = ( bh * 2.0f ) * ( bh * 2.0f ) * an[1];

                    // NaN guard: NaN in any coordinate would sail through
                    // every plain < / || filter below and land in the box list.
                    if ( cx != cx || cy != cy || w != w || h != h )
                        continue;

                    if ( cy < lb_top || cy >= lb_top + lb_h )
                        continue;  // bar

                    if ( nc < k_max_cand )
                        cand[nc++] = { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, conf, best - 5 };
                }
            }
        }
    }

    // NMS: greedy, class-aware (standard yolov5 behaviour): boxes of
    // different classes never suppress each other.
    std::sort( cand.begin(), cand.begin() + nc, []( const Box& a, const Box& b ) {
        return a.conf > b.conf;
    } );
    std::array<uint8_t, k_max_cand> sup {};
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
        Detection d{};
        d.x = std::max( 0.0f, b.x1 * scale );
        d.y = std::max( 0.0f, ( b.y1 - lb_top ) * scale );
        d.w = std::min( cam_w - d.x, ( b.x2 - b.x1 ) * scale );
        d.h = std::min( cam_h - d.y, ( b.y2 - b.y1 ) * scale );
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
