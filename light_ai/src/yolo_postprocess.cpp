#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "yolo_postprocess.hpp"
#include "math.h"
#include "rknn_engine.hpp"

namespace
{
    // fp16 (IEEE 754 half) -> float, bit-exact, no arch extensions needed.
    inline float fp16_to_float( uint16_t h )
    {
        const uint32_t sign = ( h >> 15 ) & 1u;
        uint32_t exp = ( h >> 10 ) & 31u;
        uint32_t man = h & 0x3FFu;
        uint32_t bits = 0;
        if ( exp == 0 )
        {
            if ( man == 0 )
            {
                bits = sign << 31;  // +-0
            }
            else
            {  // subnormal
                exp = 127 - 15 + 1;
                while ( !( man & 0x400u ) )
                {
                    man <<= 1;
                    --exp;
                }
                man &= 0x3FFu;
                bits = ( sign << 31 ) | ( exp << 23 ) | ( man << 13 );
            }
        }
        else if ( exp == 31 )
        {
            bits = ( sign << 31 ) | 0x7F800000u | ( man << 13 );  // +-inf/nan
        }
        else
        {
            bits = ( sign << 31 ) | ( ( exp - 15 + 127 ) << 23 ) | ( man << 13 );
        }
        float f = NAN;
        std::memcpy( &f, &bits, 4 );
        return f;
    }

    // yolov5 default anchors, already in model-input pixels per stride.
    constexpr float ANCH[3][3][2] = {
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
    constexpr int kMaxCand = 1024;
    std::array<Box, kMaxCand> cand{};
    int nc = 0;

    const float scale = cam_w / model_w;  // uniform, by width

    for ( size_t gi = 0; gi < npu.output_attrs().size() && gi < 3; ++gi )
    {
        const auto& attr = npu.output_attrs()[gi];
        const uint16_t* data = reinterpret_cast<const uint16_t*>( npu.output_data( gi ) );
        if ( !data || attr.n_dims != 4 )
            continue;

        // NCHW: dims = {1, 255, H, W}
        const int H = static_cast<int>( attr.dims[2] );
        const int W = static_cast<int>( attr.dims[3] );
        const int stride = 8 << gi;
        const int per = static_cast<int>( attr.dims[1] ) / 3;  // 85 = 4+1+80

        for ( int gy = 0; gy < H; ++gy )
        {
            for ( int gx = 0; gx < W; ++gx )
            {
                const size_t base = ( static_cast<size_t>( gy ) * W + gx );
                // channel-major: element (c, gy, gx) is at c*H*W + base
                const size_t cellHW = static_cast<size_t>( H ) * W;
                // anchor loop is merged with the confidence shortcut below
                for ( int k = 0; k < 3; ++k )
                {
                    const size_t off = base + static_cast<size_t>( k ) * per * cellHW;
                    const float obj = fp16_to_float( data[off + 4 * cellHW] );
                    if ( obj < 0.05f )
                        continue;  // cheap early cut, below any usable conf_thr

                    int best = 0;
                    float best_cls = 0.0f;
                    for ( int c = 5; c < per; ++c )
                    {
                        const float v = fp16_to_float( data[off + c * cellHW] );
                        if ( v > best_cls )
                        {
                            best_cls = v;
                            best = c;
                        }
                    }
                    const float conf = obj * best_cls;
                    if ( conf < conf_thr )
                        continue;

                    const float* an = ANCH[gi][k];
                    const float cx = ( fp16_to_float( data[off + 0 * cellHW] ) * 2.0f - 0.5f + gx ) * stride;
                    const float cy = ( fp16_to_float( data[off + 1 * cellHW] ) * 2.0f - 0.5f + gy ) * stride;
                    const float bw = fp16_to_float( data[off + 2 * cellHW] );
                    const float bh = fp16_to_float( data[off + 3 * cellHW] );
                    const float w = ( bw * 2.0f ) * ( bw * 2.0f ) * an[0];
                    const float h = ( bh * 2.0f ) * ( bh * 2.0f ) * an[1];

                    if ( cy < lb_top || cy >= lb_top + lb_h )
                        continue;  // bar

                    if ( nc < kMaxCand )
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
        Detection d{};
        d.x = std::max( 0.0f, b.x1 * scale );
        d.y = std::max( 0.0f, ( b.y1 - lb_top ) * scale );
        d.w = std::min( cam_w - d.x, ( b.x2 - b.x1 ) * scale );
        d.h = std::min( cam_h - d.y, ( b.y2 - b.y1 ) * scale );
        d.cls = b.cls;
        d.conf = b.conf;
        out.push( d );
    }
}
