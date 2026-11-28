// YoloDecoder on synthetic fp16 heads: a 64 x 64 model, 2 classes
// (per = 4 + 1 + 2 = 7 channels per anchor, ch = 21), the three strides
// 8 / 16 / 32 -> maps 8x8 / 4x4 / 2x2. Every value written into a head is
// exactly representable in fp16, so the expected boxes are exact.

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "core/yolo_decoder.hpp"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    constexpr int kPer = 7;  // 4 box + 1 obj + 2 classes
    constexpr int kCh = 3 * kPer;
    constexpr uint16_t kNaN = 0x7E00;
    constexpr uint16_t kPosInf = 0x7C00;
    constexpr uint16_t kNegInf = 0xFC00;

    // float -> fp16 for values exact in fp16 (normal range or zero); the test
    // values are all of that kind.
    uint16_t half( float f )
    {
        if ( f == 0.0f )
            return 0;
        uint32_t b = 0;
        std::memcpy( &b, &f, sizeof( b ) );
        const uint32_t sign = ( b >> 16U ) & 0x8000U;
        const uint32_t exp = ( ( b >> 23U ) & 0xFFU ) - 127U + 15U;
        const uint32_t mant = ( b >> 13U ) & 0x03FFU;
        return static_cast<uint16_t>( sign | ( exp << 10U ) | mant );
    }

    // One head of side w x h, all zeros (no candidate anywhere).
    struct Head
    {
        Head( int map_w, int map_h ) : w( map_w ), h( map_h ), data( static_cast<size_t>( kCh ) * map_w * map_h, 0 ) {}

        // Anchor k of cell (gx, gy): box offsets tx, ty, size bw, bh, obj and class scores.
        void set( int gx, int gy, int k, float tx, float ty, float bw, float bh, float obj, float c0, float c1 )
        {
            const float v[kPer] = { tx, ty, bw, bh, obj, c0, c1 };
            for ( int c = 0; c < kPer; ++c )
                at( gx, gy, k, c ) = half( v[c] );
        }
        uint16_t& at( int gx, int gy, int k, int c )
        {
            const size_t cell_hw = static_cast<size_t>( w ) * h;
            return data[static_cast<size_t>( k * kPer + c ) * cell_hw + static_cast<size_t>( gy ) * w + gx];
        }
        YoloHead view() const { return YoloHead { data.data(), kCh, h, w }; }

        int w;
        int h;
        std::vector<uint16_t> data;
    };

    // Landscape: cam 128x64 into the band rows [16, 48) of the 64x64 model: scale 2.
    YoloFrame landscape()
    {
        YoloFrame f;
        f.cam_w = 128.0f;
        f.cam_h = 64.0f;
        f.model_w = 64.0f;
        f.model_h = 64.0f;
        f.lb = LbGeom { 0, 64, 16, 32 };
        return f;
    }

    // Portrait: cam 64x128 into the band columns [16, 48): scale 2.
    YoloFrame portrait()
    {
        YoloFrame f;
        f.cam_w = 64.0f;
        f.cam_h = 128.0f;
        f.model_w = 64.0f;
        f.model_h = 64.0f;
        f.lb = LbGeom { 16, 32, 0, 64 };
        return f;
    }

    bool same( const Detection& d, float x, float y, float w, float h, int cls, float conf )
    {
        return d.x == x && d.y == y && d.w == w && d.h == h && d.cls == cls && d.conf == conf;
    }

    // The reference anchor: stride 8, cell (3, 4), anchor 0 (10 x 13 px):
    // cx = (0.5 * 2 - 0.5 + 3) * 8 = 28, cy = 36, a 10 x 13 box ->
    // corners (23, 29.5) - (33, 42.5); conf = 0.75 * 1.0, class 0.
    void put_reference( Head& p3 )
    {
        p3.set( 3, 4, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.75f, 1.0f, 0.5f );
    }

    void test_single_box( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 8, 8 );
        put_reference( p3 );
        const YoloHead heads[1] = { p3.view() };
        DetList dets;
        dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );  // stale entry: cleared at entry

        dec.decode( heads, 1, landscape(), dets );
        CHECK( t, dets.n == 1 );
        // x = 23 * 2, y = (29.5 - 16) * 2, w = 10 * 2, h = 13 * 2
        CHECK( t, dets.n == 1 && same( dets.d[0], 46.0f, 27.0f, 20.0f, 26.0f, 0, 0.75f ) );

        dec.decode( heads, 1, portrait(), dets );
        CHECK( t, dets.n == 1 );
        // x = (23 - 16) * 2, y = 29.5 * 2
        CHECK( t, dets.n == 1 && same( dets.d[0], 14.0f, 59.0f, 20.0f, 26.0f, 0, 0.75f ) );
        CHECK( t, err.str().empty() );
    }

    // Each gate alone rejects the reference anchor.
    void test_gates( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        DetList dets;
        const auto run = [&]( Head& p3 ) {
            const YoloHead heads[1] = { p3.view() };
            dec.decode( heads, 1, landscape(), dets );
            return dets.n;
        };

        Head low_obj( 8, 8 );
        low_obj.set( 3, 4, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.125f, 1.0f, 0.5f );
        CHECK( t, run( low_obj ) == 0 );

        Head no_class( 8, 8 );
        no_class.set( 3, 4, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.75f, 0.0f, 0.0f );
        CHECK( t, run( no_class ) == 0 );

        Head low_conf( 8, 8 );  // 0.5 * 0.25 = 0.125 < 0.25
        low_conf.set( 3, 4, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.25f, 0.0f );
        CHECK( t, run( low_conf ) == 0 );

        Head nan_obj( 8, 8 );
        put_reference( nan_obj );
        nan_obj.at( 3, 4, 0, 4 ) = kNaN;
        CHECK( t, run( nan_obj ) == 0 );

        Head nan_xy( 8, 8 );
        put_reference( nan_xy );
        nan_xy.at( 3, 4, 0, 0 ) = kNaN;
        CHECK( t, run( nan_xy ) == 0 );

        Head nan_size( 8, 8 );
        put_reference( nan_size );
        nan_size.at( 3, 4, 0, 3 ) = kNaN;
        CHECK( t, run( nan_size ) == 0 );

        Head inf_obj( 8, 8 );
        put_reference( inf_obj );
        inf_obj.at( 3, 4, 0, 4 ) = kPosInf;
        CHECK( t, run( inf_obj ) == 0 );

        // M-14: a +Inf class channel never wins -- the finite best class
        // (class 1, 0.5) survives: conf = 0.75 * 0.5. Was: +Inf took the
        // anchor and the isfinite gate then dropped it whole.
        Head inf_cls( 8, 8 );
        put_reference( inf_cls );
        inf_cls.at( 3, 4, 0, 5 ) = kPosInf;
        CHECK( t, run( inf_cls ) == 1 );
        CHECK( t, dets.n == 1 && dets.d[0].cls == 1 && dets.d[0].conf == 0.375f );

        Head nonfinite_cls( 8, 8 );  // no finite class channel at all: no candidate
        put_reference( nonfinite_cls );
        nonfinite_cls.at( 3, 4, 0, 5 ) = kPosInf;
        nonfinite_cls.at( 3, 4, 0, 6 ) = kNaN;
        CHECK( t, run( nonfinite_cls ) == 0 );

        Head zero_size( 8, 8 );  // bw = 0: an empty box carries no geometry
        put_reference( zero_size );
        zero_size.at( 3, 4, 0, 2 ) = 0;
        CHECK( t, run( zero_size ) == 0 );

        Head inf_xy( 8, 8 );
        put_reference( inf_xy );
        inf_xy.at( 3, 4, 0, 0 ) = kNegInf;
        CHECK( t, run( inf_xy ) == 0 );

        Head inf_size( 8, 8 );
        put_reference( inf_size );
        inf_size.at( 3, 4, 0, 2 ) = kPosInf;
        CHECK( t, run( inf_size ) == 0 );

        Head bar( 8, 8 );  // cy = (1 - 0.5 + 0) * 8 = 4: above the band [16, 48)
        bar.set( 3, 0, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.75f, 1.0f, 0.0f );
        CHECK( t, run( bar ) == 0 );

        Head kept( 8, 8 );  // the control: the reference anchor passes
        put_reference( kept );
        CHECK( t, run( kept ) == 1 );
        CHECK( t, err.str().empty() );
    }

    // Two anchors of one cell, the second a 9 x 7.5 box inside the 10 x 13 one:
    // IoU = 67.5 / 130 = 0.52 > 0.45. The same class: the weaker one goes;
    // another class: both stay, in confidence order.
    void test_nms( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        DetList dets;

        Head same_cls( 8, 8 );
        same_cls.set( 3, 4, 1, 0.5f, 0.5f, 0.375f, 0.25f, 0.5f, 1.0f, 0.0f );  // anchor 1 (16 x 30): 9 x 7.5
        put_reference( same_cls );
        const YoloHead h1[1] = { same_cls.view() };
        dec.decode( h1, 1, landscape(), dets );
        CHECK( t, dets.n == 1 && dets.d[0].conf == 0.75f );

        Head other_cls( 8, 8 );
        other_cls.set( 3, 4, 1, 0.5f, 0.5f, 0.375f, 0.25f, 0.5f, 0.0f, 1.0f );
        put_reference( other_cls );
        const YoloHead h2[1] = { other_cls.view() };
        dec.decode( h2, 1, landscape(), dets );
        CHECK( t, dets.n == 2 );
        CHECK( t, dets.n == 2 && dets.d[0].conf == 0.75f && dets.d[0].cls == 0 );
        CHECK( t, dets.n == 2 && dets.d[1].conf == 0.5f && dets.d[1].cls == 1 );
    }

    // The stride comes from the map size: the heads in P5 / P4 / P3 order give
    // the same detections as in P3 / P4 / P5 order.
    void test_head_order( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 8, 8 );
        Head p4( 4, 4 );
        Head p5( 2, 2 );
        put_reference( p3 );                                                  // conf 0.75
        p4.set( 1, 2, 2, 0.5f, 0.5f, 0.25f, 0.25f, 0.5f, 1.0f, 0.0f );       // conf 0.5, stride 16
        p5.set( 1, 1, 0, 0.25f, 0.25f, 0.25f, 0.25f, 1.0f, 0.0f, 0.625f );  // conf 0.625, stride 32

        DetList fwd;
        const YoloHead in_order[3] = { p3.view(), p4.view(), p5.view() };
        dec.decode( in_order, 3, landscape(), fwd );
        DetList rev;
        const YoloHead reversed[3] = { p5.view(), p4.view(), p3.view() };
        dec.decode( reversed, 3, landscape(), rev );

        CHECK( t, fwd.n == 3 && rev.n == 3 );
        bool equal = fwd.n == rev.n;
        for ( int i = 0; equal && i < fwd.n; ++i )
            equal = same( rev.d[i], fwd.d[i].x, fwd.d[i].y, fwd.d[i].w, fwd.d[i].h, fwd.d[i].cls, fwd.d[i].conf );
        CHECK( t, equal );
        CHECK( t, fwd.n == 3 && fwd.d[0].conf == 0.75f && fwd.d[1].conf == 0.625f && fwd.d[2].conf == 0.5f );
        // P4: cx = (1 - 0.5 + 1) * 16 = 24, cy = (1 - 0.5 + 2) * 16 = 40, anchor 2 (59 x 119)
        // at 0.25 -> 14.75 x 29.75: corners (16.625, 25.125) - (31.375, 54.875); the
        // bottom edge is clipped to the band (cam_h - y).
        CHECK( t, fwd.n == 3 && same( fwd.d[2], 33.25f, 18.25f, 29.5f, 45.75f, 0, 0.5f ) );
        CHECK( t, err.str().empty() );
    }

    // Rejected heads are logged and skipped, the valid one still decodes; a
    // null head is skipped silently.
    void test_bad_heads( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 8, 8 );
        put_reference( p3 );
        const Head odd( 5, 5 );    // 64 % 5 != 0
        const Head s4( 16, 16 );   // stride 4: no anchor set
        YoloHead bad_layout = p3.view();
        bad_layout.ch = 20;  // not 3 anchors
        const YoloHead heads[3] = { odd.view(), s4.view(), YoloHead {} };
        const YoloHead heads2[2] = { bad_layout, p3.view() };

        DetList dets;
        dec.decode( heads, 3, landscape(), dets );
        CHECK( t, dets.n == 0 );
        CHECK( t, err.str() == "yolo: head 0 width 5 does not divide model 64\n"
                               "yolo: head 1 unsupported stride 4\n" );
        err.str( "" );
        dec.decode( heads2, 2, landscape(), dets );
        CHECK( t, dets.n == 1 );
        CHECK( t, err.str() == "yolo: unexpected head layout (per=6, ch=20)\n" );
    }

    // A band that does not fit the model: an empty list and one message.
    void test_band_misfit( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 8, 8 );
        put_reference( p3 );
        const YoloHead heads[1] = { p3.view() };
        YoloFrame f = landscape();
        f.lb = LbGeom { 0, 64, 40, 32 };  // rows [40, 72) of a 64-row model
        DetList dets;
        dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );
        dec.decode( heads, 1, f, dets );
        CHECK( t, dets.n == 0 );
        CHECK( t, err.str() == "yolo_postprocess: letterbox band x=[0, 64) y=[40, 72) does not fit model 64x64\n" );
    }

    // No heads, or a model size the casts of decode() cannot take: an empty list.
    void test_bad_frame( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 8, 8 );
        put_reference( p3 );
        const YoloHead heads[1] = { p3.view() };
        DetList dets;
        dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );
        dec.decode( nullptr, 1, landscape(), dets );
        CHECK( t, dets.n == 0 );
        dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );
        dec.decode( heads, 0, landscape(), dets );
        CHECK( t, dets.n == 0 && err.str().empty() );
        dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );
        dec.decode( heads, -1, landscape(), dets );
        CHECK( t, dets.n == 0 && err.str().empty() );

        constexpr float inf = std::numeric_limits<float>::infinity();
        constexpr float nan = std::numeric_limits<float>::quiet_NaN();
        for ( const float bad : { inf, nan, -64.0f, 0.0f, 65537.0f, 1.0e30f } )
        {
            YoloFrame wide = landscape();
            wide.model_w = bad;
            dets.push( Detection { 1, 2, 3, 4, 5, 0.5f } );
            dec.decode( heads, 1, wide, dets );
            CHECK( t, dets.n == 0 );
            YoloFrame tall = landscape();
            tall.model_h = bad;
            dec.decode( heads, 1, tall, dets );
            CHECK( t, dets.n == 0 );
        }
        CHECK( t, err.str().find( "does not fit model infx64" ) != std::string::npos );
        dec.decode( heads, 1, landscape(), dets );
        CHECK( t, dets.n == 1 );
    }

    // Edge-clipped person whose center sits slightly in the gray bar (4 px past top) with
    // visible overlap is recovered and clamped cleanly to the scene edge.
    void test_edge_tolerance( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p5( 2, 2 );
        // Stride 32, gy=0: cy = (0.75 * 2 - 0.5 + 0) * 32 = 32? Pick ty=0.35 -> cy=(0.7-0.5)*32=6.4,
        // inside band [16, 48) margin (16-9.6=6.4 vs center off by 9.6 <= pad 16), anchor 2 (h=326 -> bh=32.6).
        // y1 = 6.4 - 16.3 = -9.9, y2 = 6.4 + 16.3 = 22.7 -> overlaps [16, 48) by 6.7 px!
        p5.set( 1, 0, 2, 0.5f, 0.35f, 0.25f, 0.158f, 0.8f, 1.0f, 0.0f );
        const YoloHead heads[1] = { p5.view() };
        DetList dets;
        dec.decode( heads, 1, landscape(), dets );
        CHECK( t, dets.n == 1 );
        CHECK_NEAR( t, dets.d[0].y, 0.0f, 1e-3f );
        CHECK( t, dets.d[0].h > 10.0f );
        CHECK( t, err.str().empty() );
    }

    // Equal-confidence candidates preserve scan order (stable_sort) and DetList::cap overflow warns once.
    void test_det_cap_overflow( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 32, 8 );  // 256 cells -> 256 non-overlapping candidates (cell spacing 8 > box size 4)
        for ( int gy = 0; gy < 8; ++gy )
            for ( int gx = 0; gx < 32; ++gx )
                p3.set( gx, gy, 0, 0.5f, 0.5f, 0.25f, 0.25f, 0.8f, 1.0f, 0.0f );
        YoloFrame f;
        f.cam_w = 256.0f;
        f.cam_h = 64.0f;
        f.model_w = 256.0f;
        f.model_h = 64.0f;
        f.lb = LbGeom { 0, 256, 0, 64 };
        const YoloHead heads[1] = { p3.view() };
        DetList dets;
        dec.decode( heads, 1, f, dets );
        CHECK( t, dets.n == DetList::cap );
        bool scan_order = true;
        for ( int k = 1; k < dets.n; ++k )
        {
            const Detection& prev = dets.d[k - 1];
            const Detection& cur = dets.d[k];
            if ( cur.y < prev.y || ( cur.y == prev.y && cur.x <= prev.x ) )
                scan_order = false;
        }
        CHECK( t, scan_order );
        CHECK( t, err.str() == "yolo: det cap 128 reached, 128 detections dropped\n" );
    }

    // More candidates than kMaxCand: the overflow is counted and reported once.
    // A 1024 x 128 model, stride 8: 128 x 16 cells x 3 anchors = 6144 candidates.
    void test_cap( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 128, 16 );
        for ( int gy = 0; gy < 16; ++gy )
            for ( int gx = 0; gx < 128; ++gx )
                for ( int k = 0; k < 3; ++k )
                    p3.set( gx, gy, k, 0.5f, 0.5f, 0.5f, 0.5f, 0.75f, 1.0f, 0.0f );
        YoloFrame f;
        f.cam_w = 1024.0f;
        f.cam_h = 128.0f;
        f.model_w = 1024.0f;
        f.model_h = 128.0f;
        f.lb = LbGeom { 0, 1024, 0, 128 };
        const YoloHead heads[1] = { p3.view() };
        DetList dets;
        dec.decode( heads, 1, f, dets );
        CHECK( t, dets.n == DetList::cap );
        CHECK( t, err.str().find( "yolo: candidate cap 4096 reached, 2048 candidates dropped (raise --conf)\n" ) == 0 );
        CHECK( t, err.str().find( "yolo: det cap 128 reached," ) != std::string::npos );
    }

    // Scanning P5 -> P4 -> P3 ensures > kMaxCand noise on P3 does not drop a P5 detection.
    void test_p5_survives_p3_overflow( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 128, 16 );
        for ( int gy = 0; gy < 16; ++gy )
            for ( int gx = 0; gx < 128; ++gx )
                for ( int k = 0; k < 3; ++k )
                    p3.set( gx, gy, k, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.0f );
        Head p5( 32, 4 );
        p5.set( 16, 2, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.875f, 0.0f, 1.0f );

        YoloFrame f;
        f.cam_w = 1024.0f;
        f.cam_h = 128.0f;
        f.model_w = 1024.0f;
        f.model_h = 128.0f;
        f.lb = LbGeom { 0, 1024, 0, 128 };
        // Pass P3 first in input array: decoder still scans P5 before P3.
        const YoloHead heads[2] = { p3.view(), p5.view() };
        DetList dets;
        dec.decode( heads, 2, f, dets );
        CHECK( t, dets.n > 0 );
        CHECK( t, dets.n > 0 && dets.d[0].cls == 1 && dets.d[0].conf == 0.875f );
        CHECK( t, err.str().find( "yolo: candidate cap 4096 reached, 2049 candidates dropped" ) == 0 );
    }

    // Candidates above kNmsTopK (384) are capped cleanly before O(N^2) NMS.
    void test_nms_topk_cap( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        YoloDecoder dec( 0.25f, 0.45f, log );
        Head p3( 128, 32 );
        // 24 x 16 = 384 cells whose large class-0 boxes all overlap (IoU > 0.80).
        for ( int gy = 0; gy < 16; ++gy )
            for ( int gx = 0; gx < 24; ++gx )
                if ( gx != 23 || gy != 15 )
                    p3.set( gx, gy, 0, 0.5f, 0.5f, 8.0f, 8.0f, 0.75f, 1.0f, 0.0f );
        // 384th candidate: lower-confidence class 1 (survives when total == kNmsTopK).
        p3.set( 100, 20, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 1.0f );

        YoloFrame f;
        f.cam_w = 1024.0f;
        f.cam_h = 256.0f;
        f.model_w = 1024.0f;
        f.model_h = 256.0f;
        f.lb = LbGeom { 0, 1024, 0, 256 };
        const YoloHead heads[1] = { p3.view() };
        DetList dets;
        dec.decode( heads, 1, f, dets );
        CHECK( t, dets.n == 2 );
        CHECK( t, dets.n == 2 && dets.d[0].cls == 0 && dets.d[1].cls == 1 );

        // Adding one more high-conf class-0 box (385 total > kNmsTopK) drops the
        // lower-confidence class-1 box before NMS; the 384 overlapping class-0 boxes
        // collapse to 1 survivor.
        p3.set( 23, 15, 0, 0.5f, 0.5f, 8.0f, 8.0f, 0.75f, 1.0f, 0.0f );
        dec.decode( heads, 1, f, dets );
        CHECK( t, dets.n == 1 );
        CHECK( t, dets.n == 1 && dets.d[0].cls == 0 && dets.d[0].conf == 0.75f );
        CHECK( t, err.str().empty() );
    }
}  // namespace

void test_yolo_decoder( TestRun& t )
{
    test_single_box( t );
    test_gates( t );
    test_nms( t );
    test_head_order( t );
    test_bad_heads( t );
    test_band_misfit( t );
    test_bad_frame( t );
    test_edge_tolerance( t );
    test_det_cap_overflow( t );
    test_cap( t );
    test_p5_survives_p3_overflow( t );
    test_nms_topk_cap( t );
}
