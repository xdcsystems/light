#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>  // stat(): startup node fallback
#include <unistd.h>
#include <cmath>

#include "pipeline.hpp"
#include "coco_names.h"
#include "yolo_postprocess.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    inline double ms( clk::time_point a, clk::time_point b ) { return std::chrono::duration<double, std::milli>( b - a ).count(); }

    struct Timings
    {
        double cam = 0, dec = 0, rga = 0, infer = 0, enc = 0;  // sums, ms
        int n = 0;
        // Pool accounting: in pool mode the per-stage sums double-count
        // the in-flight window overlap (cam wait of frame k vs infer of
        // frame k-2), so sum -> FPS is wrong; FPS comes from the wall
        // clock of the whole run instead. CFR (pool 1) keeps the legacy
        // honest accounting.
        clk::time_point t0{};  // first frame stamp
        long total = 0;        // frames since start
        int pool = 1;          // in-flight frames; 1 = legacy path

        void set_pool( int p )
        {
            if ( p > 1 )
                pool = p;
        }

        void add( const FrameTrace& tr )
        {
            if ( total == 0 )
                t0 = clk::now();
            ++total;
            cam += tr.cam;
            dec += tr.dec;
            rga += tr.rga;
            infer += tr.infer;
            enc += tr.enc;
            if ( ++n % 60 == 0 )
                report( "avg/60" );
        }

        void report( const char* tag )
        {
            if ( n == 0 )
                return;
            const double sum = ( cam + dec + rga + infer + enc ) / n;
            if ( pool > 1 )
            {
                const double wall = total / std::chrono::duration<double>( clk::now() - t0 ).count();
                std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                          << " rga=" << rga / n << " infer=" << infer / n << " enc=" << enc / n << " | sum=" << sum
                          << " ms (pool window latency, " << pool << " in flight) | wall ~" << wall << " FPS\n";
            }
            else
            {
                std::cout << "[timing] " << tag << " cam=" << std::fixed << std::setprecision( 1 ) << cam / n << " dec=" << dec / n
                          << " rga=" << rga / n << " infer=" << infer / n << " enc=" << enc / n << " | sum=" << sum << " ms -> ~"
                          << ( sum > 0 ? 1000.0 / sum : 0.0 ) << " FPS\n";
            }
            cam = dec = rga = infer = enc = 0;
            n = 0;
        }
    } g_t;

    // Colors shared by the GPU and CPU drawing paths (owner's pick).
    inline void det_color( int cls, uint8_t& r, uint8_t& g, uint8_t& b )
    {
        switch ( cls )
        {
            case 0:
                r = 255;
                g = 64;
                b = 64;
                break;  // person
            case 56:
                r = 64;
                g = 255;
                b = 64;
                break;  // chair
            default:
                r = 255;
                g = 255;
                b = 0;
                break;  // others
        }
    }

    // M3.7 CPU baseline: 1-px outlines straight into the NPU tensor (RGB888).
    // The caller pre-maps boxes to tensor coordinates — the same geometry as
    // the GPU path; colors come from the box itself. Used when the GPU drawer
    // is unavailable, or forced by --cpu-draw (A/B baseline).
    void draw_boxes_cpu( uint8_t* rgb, int w, int h, int stride, const GpuBox* boxes, int n )
    {
        auto clamp = []( int v, int hi ) {
            return v < 0 ? 0 : ( v > hi ? hi : v );
        };
        for ( int i = 0; i < n; ++i )
        {
            const GpuBox& b = boxes[i];
            const int x0 = clamp( static_cast<int>( b.x0 ), w - 1 );
            const int y0 = clamp( static_cast<int>( b.y0 ), h - 1 );
            const int x1 = clamp( static_cast<int>( b.x1 ), w - 1 );
            const int y1 = clamp( static_cast<int>( b.y1 ), h - 1 );
            auto put = [&]( int x, int y ) {
                uint8_t* px = rgb + static_cast<size_t>( y ) * stride + x * 3;
                px[0] = b.r;
                px[1] = b.g;
                px[2] = b.b;
            };
            for ( int x = x0; x <= x1; ++x )
            {
                put( x, y0 );
                put( x, y1 );
            }
            for ( int y = y0; y <= y1; ++y )
            {
                put( x0, y );
                put( x1, y );
            }
        }
    }

}  // namespace

Pipeline::Pipeline( const Config& cfg )
  : cfg_( cfg ), cam_( cfg_.cam_w, cfg_.cam_h ), dec_( cfg_.cam_w, cfg_.cam_h )
{
    // M4: the pool is a startup-time decision; npus_ never grows after
    // this loop, so the slot pointers stay valid for the whole session.
    npool_ = cfg_.pool >= 3 ? 3 : 1;
    npus_.reserve( npool_ );
    for ( int k = 0; k < npool_; ++k )
        npus_.emplace_back( cfg_.model.c_str() );

    cam_node_ = cfg_.device;
    // Startup fallback for a re-enumerated camera: the configured node
    // may be gone after a replug (verified live: video20 -> video21)
    // while the camera itself is alive. ENOENT-only: if the node exists
    // but setup fails (busy, permissions, wrong node type), the error
    // stays fatal — a silently substituted source is worse than a loud
    // one. find_usb_capture_node() skips a missing preferred path on
    // its own (stat) and trial-setups the candidates, so fake nodes
    // (metadata-only) are filtered there.
    if ( !cam_node_.empty() )
    {
        struct stat st
        {};
        if ( stat( cam_node_.c_str(), &st ) != 0 )
        {
            std::cerr << "camera: configured node " << cam_node_ << " not present, falling back to USB discovery\n";
            std::string found;
            if ( find_usb_capture_node( cam_, cam_node_, found ) != 0 )
                throw std::runtime_error( "camera: configured node " + cam_node_ + " is missing and no USB capture node found" );
            std::cout << "camera: discovery adopted " << found << '\n';
            cam_node_ = found;
        }
    }

    std::string cam_why;
    if ( cam_.setup_device( cam_node_, &cam_why ) != 0 )
    {
        throw std::runtime_error( "camera: " + cam_why );
    }
    for ( auto& e : npus_ )
    {
        if ( e.setup_io() != 0 )
        {
            throw std::runtime_error( "rknn setup_io failed" );
        }
    }
    // M2.5/M4: core pinning. The single path keeps cfg_.core verbatim
    // (AUTO by default — the A/B baseline is untouched). The pool pins
    // its own: one engine per core, via the per-core flags 1/2/4.
    if ( npool_ == 1 )
    {
        if ( cfg_.core != 0 )
        {
            if ( npus_[0].set_core_mask( static_cast<unsigned>( cfg_.core ) ) != 0 )
            {
                throw std::runtime_error( "rknn set_core_mask failed" );
            }
            std::cout << "npu: core mask 0x" << std::hex << cfg_.core << std::dec << "\n";
        }
    }
    else
    {
        constexpr int cores[] = { 1, 2, 4 };
        if ( npool_ > static_cast<int>( std::size( cores )) )
        {
            throw std::runtime_error( "npu pool size > 3 is not supported" );
        }

        for ( int k = 0; k < npool_; ++k )
        {
            // clang 11 analyzer cannot fold std::size() into a constant and misses the
            // guard above; reading kCores[k] is safe because k < npool_ <= std::size(kCores).
            // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
            if ( npus_[k].set_core_mask( static_cast<unsigned>( cores[k] ) ) != 0 )
            {
                throw std::runtime_error( "rknn set_core_mask failed" );
            }
        }
        std::cout << "npu: pool of " << npool_ << " engines, one per core\n";
        g_t.set_pool( npool_ );
    }
    // Single source of truth for the model geometry: the model file itself.
    // NHWC layout: dims = {N, H, W, C}, so W is dims[n-2] and H is dims[n-3].
    const rknn_tensor_attr& ia = npus_[0].input_attr();  // all engines share the model geometry
    if ( ia.n_dims < 4 )
    {
        std::cerr << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
        throw std::runtime_error( "rknn: input tensor is not 4-D NHWC" );
    }

    // Gray letterbox bars: RGA writes only the visible band (drect), so the
    // 128-fill done here survives every frame.
    // M4: gray letterbox bars on every pooled tensor — RGA writes only
    // the visible band (drect), so the 128-fill survives every frame.
    for ( auto& e : npus_ )
        std::memset( e.input_data(), 128, e.input_size() );

    model_w_ = static_cast<int>( ia.dims[ia.n_dims - 2] );
    model_h_ = static_cast<int>( ia.dims[ia.n_dims - 3] );

    if ( cfg_.net )
    {
        // Hard confirmation of tensor contiguity: if size_with_stride equals
        // w*h*3, the buffer is exactly the PPM dump layout (stride = w*3).
        std::cout << "net: tensor stride check: size_with_stride=" << ia.size_with_stride << " vs " << model_w_ * model_h_ * 3 << "\n";
        if ( cfg_.net_h264 )
        {
            // M3.5 pull: empty net_host = TCP pull server (listener
            // on net_port, the viewer connects to the board's LAN
            // IP); a non-empty net_host keeps the legacy UDP push
            // to that destination.
            // M3.5: the h264 encoder consumes the NPU input tensor AFTER
            // box drawing, at the NATIVE letterbox-band size (no upscale):
            // w = model_w, h = cam_h * (model_w / cam_w) — for the default
            // 1280x720 camera and a 640x640 tensor that is 640x360 (16:9).
            // The encoder derives the same band geometry from these numbers.

            const float s = static_cast<float>( model_w_ ) / cfg_.cam_w;
            const int band_h = static_cast<int>( lroundf( cfg_.cam_h * s ) );

            enc_.emplace( model_w_, band_h, cfg_.net_fps, cfg_.net_port, cfg_.net_host, cfg_.net_bitrate, model_w_, model_h_ );
            if ( cfg_.net_host.empty() )
            {
                std::cout << "net: h264 PULL server on port " << cfg_.net_port << ", " << cfg_.net_bitrate
                          << " kbps (viewer: ffplay tcp://<board-ip>:" << cfg_.net_port << ")\n";
            }
            else
            {
                std::cout << "net: h264 preview -> " << cfg_.net_host << ':' << cfg_.net_port << ", " << cfg_.net_bitrate << " kbps\n";
            }
        }
        else
        {
            preview_.emplace( model_w_, model_h_, model_w_ * 3, cfg_.net_fps, cfg_.net_port );
            std::cout << "net: preview server on port " << cfg_.net_port << "\n";
        }
    }

    // M3.7: GPU box drawing (Mali G610, EGL/GBM) straight into the NPU
    // tensor. Init once, only with a preview sink active. On failure the
    // CPU path (net_preview::draw_boxes_rgb24) stays as the fallback.
    // --cpu-draw forces the CPU path (A/B baseline).
    if ( ( preview_ || enc_ ) && !cfg_.cpu_draw )
    {
        drawer_.emplace();
        // M4: one EGLImage per slot, attached once at startup (attach_slot
        // is idempotent per slot); draw switches FBOs between slots.
        bool drew_ok = drawer_->init();
        for ( int k = 0; drew_ok && k < npool_; ++k )
        {
            drew_ok = drawer_->attach_slot( k, npus_[k].input_fd(), model_w_, model_h_, static_cast<uint32_t>( model_w_ ) * 3 );
        }
        if ( !drew_ok )
        {
            std::cerr << "pipeline: gpu drawer unavailable, RGA fallback\n";
            drawer_.reset();
        }
        else
        {
            std::cout << "net: gpu box drawing active (Mali G610, EGL/GBM)\n";
        }
    }

    // Aux buffer only for the two-pass path (source over 8x the model input).
    // 1280x720 -> 224 is 5.7x — single pass, aux not allocated, no DRM pool
    // split. 1920x1080 still needs the 960x540 intermediate.
    if ( cfg_.cam_w > 8 * model_w_ || cfg_.cam_h > 8 * model_h_ )
    {
        aux_fd_ = dec_.aux_fd( static_cast<size_t>( cfg_.cam_w / 2 ) * ( cfg_.cam_h / 2 ) * 2 );
        if ( aux_fd_ < 0 )
        {
            throw std::runtime_error( "pipeline: aux buffer alloc failed" );
        }
    }
    else
    {
        std::cout << "aux not needed: " << cfg_.cam_w << 'x' << cfg_.cam_h << " fits single-pass into " << model_w_ << 'x' << model_h_
                  << '\n';
    }

    if ( cfg_.cal_frames > 0 )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
        {
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
        }
    }
    // M4: bind slots to engines, then start the worker threads (pool
    // only — with a single engine the legacy synchronous path runs).
    for ( int k = 0; k < npool_; ++k )
        slots_[k].eng = &npus_[k];
    for ( int k = 0; k < npool_; ++k )
    {
        workers_.emplace_back( [this, k] {
            worker_loop( k );
        } );
    }

    cam_.start();
}

int Pipeline::run()
{
    int ok = 0;
    const auto t_start = clk::now();

    // Disconnect policy: ENODEV/EIO from DQBUF mean the device is gone
    // (unplug, USB hiccup) — worth a few timed reconnect attempts; any
    // other errno is a hard error and ends the run with exit code 1
    // (run() returns -1, main() maps it to 1). Exhausted reconnect
    // attempts end the same way; a silent exit 0 on camera death is the
    // bug this code removes. A recovery attempt consumes one frame of
    // the --frames budget and contributes nothing to the timing stats.
    // 20 attempts: observed USB re-enumeration on this board can take
    // longer than the original 10x1s window allowed.
    constexpr int k_cam_reconnect_max = 20;
    constexpr auto k_cam_reconnect_pause = std::chrono::seconds( 1 );
    constexpr int k_warmup_frames = 3;
    int reconnects = 0;
    int warmup_left = 0;  // publish-discards after reconnect

    smoother_.set_log( cfg_.box_log );
    for ( int i = 0; i < cfg_.frames && !stop_.load(); ++i )
    {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        int cam_err = 0;
        const int idx = cam_.dequeue( &sz, &cam_err );
        if ( idx < 0 )
        {
            // Classify the failure before acting on it.
            const bool recoverable = ( cam_err == ENODEV || cam_err == EIO );
            std::cerr << "camera: dequeue failed: " << strerror( cam_err ) << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
            if ( !recoverable )
                return -1;
            bool back = false;
            for ( int a = 1; a <= k_cam_reconnect_max; ++a )
            {
                std::this_thread::sleep_for( k_cam_reconnect_pause );
                std::string node;
                if ( find_usb_capture_node( cam_, cam_node_, node ) != 0 )
                    continue;
                std::string why;
                if ( cam_.reconnect( node, &why ) == 0 )
                {
                    if ( node != cam_node_ )
                    {
                        std::cout << "camera: re-enumerated node adopted: " << cam_node_ << " -> " << node << '\n';
                        cam_node_ = node;
                    }
                    ++reconnects;
                    warmup_left = k_warmup_frames;
                    std::cout << "camera: reconnected on attempt " << a << '\n';
                    // M4: frames from the dead stream are never published —
                    // drop the pending slots (their infer may still be in
                    // flight; wait for DONE, then free without publishing).
                    if ( npool_ > 1 )
                        pool_discard();
                    back = true;
                    break;
                }
                std::cerr << "camera: reconnect on " << node << " failed: " << why << '\n';
            }
            if ( !back )
            {
                std::cerr << "camera: reconnect failed after " << k_cam_reconnect_max << " attempts, giving up\n";
                return -1;
            }
            continue;
        }
        const auto t1 = clk::now();

        // Zero-size packet guard: a dying/reconnecting camera can hand
        // out a buffer with bytesused == 0. Feeding it to the decoder
        // produces "BAD INPUT ok=1 ... size=0" — MPP reports success on
        // an EMPTY packet, and the buffer behind the returned fd is
        // stale/uninitialized memory. RGA then stretches that garbage
        // into the NPU tensor, YOLO hallucinates on it and the stream
        // shows phantom boxes (observed right after a replug). Skip the
        // decode, keep the buffer cycle intact (requeue still runs).
        int fd_src = -1;
        if ( sz > 0 )
        {
            fd_src = dec_.decode( static_cast<const uint8_t*>( cam_.buffer( idx ) ), sz );
        }
        else
        {
            std::cerr << "camera: zero-size frame skipped\n";
        }
        cam_.requeue( idx );
        const auto t2 = clk::now();

        // A frame that failed to decode contributes neither to ok nor to the
        // timing statistics — otherwise rare errors would silently skew averages.
        if ( fd_src < 0 )
            continue;

        // Warm-up discard: even non-empty first packets after STREAMON
        // start mid-stream, so the first decoded frame(s) after a
        // reconnect can be partial garbage. They ARE decoded (flushing
        // the decoder is the point) but never inferred or published —
        // a two-frame gap beats phantoms on a live stream. The counter
        // counts VALID decodes only: garbage frames (fd < 0) do not
        // consume it, so publishing resumes on real data.
        if ( warmup_left > 0 )
        {
            --warmup_left;
            if ( warmup_left == 0 )
                std::cout << "camera: warm-up done, publishing resumed\n";
            continue;
        }

        // ---- M4 pool path: ring of slots, one per engine ----
        // Publish-before-reuse: when the ring is full, the oldest frame is
        // published first (waiting for its worker), which frees exactly the
        // slot RGA is about to overwrite. Frame order stays strictly
        // sequential (BoxSmoother semantics unchanged); timings move to the
        // publish step — the trailing g_t.add below runs for the single
        // path only.
        if ( npool_ > 1 )
        {
            const double cam_ms = ms( t0, t1 );
            const double dec_ms = ms( t1, t2 );
            if ( pub_n_ == npool_ )
                ok += publish_oldest();
            int slot = -1;
            for ( int k = 0; k < npool_; ++k )
            {
                std::lock_guard<std::mutex> lk( slots_[k].mx );
                if ( slots_[k].st == Slot::FREE )
                {
                    slot = k;
                    break;
                }
            }
            // Ring depth equals the pool size: after publishing the oldest
            // frame a FREE slot is guaranteed to exist.
            Slot& sl = slots_[slot];
            const auto t3 = clk::now();
            LbGeom lg {};
            const bool rga_ok =
                    rga_.resize( fd_src, cfg_.cam_w, cfg_.cam_h, sl.eng->input_fd(), model_w_, model_h_, aux_fd_, cfg_.cam_w / 2, cfg_.cam_h / 2, &lg );
            cal_dump_maybe( *sl.eng, i );
            const auto t3b = clk::now();
            const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t0 - t_start ).count();
            dispatch( slot, i, ts_us, rga_ok, lg.top, lg.h, cam_ms, dec_ms, ms( t3, t3b ) );
            continue;
        }

        LbGeom lg {};
        const bool rga_ok =
                rga_.resize( fd_src, cfg_.cam_w, cfg_.cam_h, npus_[0].input_fd(), model_w_, model_h_, aux_fd_, cfg_.cam_w / 2, cfg_.cam_h / 2, &lg );

        const auto t3 = clk::now();

        cal_dump_maybe( npus_[0], i );

        const auto t3b = clk::now();  // cal-dump file I/O is not infer

        const bool npu_ok = rga_ok && ( npus_[0].run() == 0 );
        const auto t4 = clk::now();
        double enc_ms = 0.0;  // measured below, inside the publish branch

        if ( npu_ok )
        {
            // Letterbox geometry from the single source of truth: RgaResizer
            // stored it during the last resize.
            yolo_postprocess( npus_[0],
                    cfg_.cam_w,
                    cfg_.cam_h,
                    model_w_,
                    model_h_,
                    static_cast<float>( lg.top ),
                    static_cast<float>( lg.h ),
                    dets_raw_,
                    cfg_.conf_thr,
                    cfg_.iou_thr );
            // M3.10: EMA smoothing on scene-coord boxes; the raw list
            // is kept only for the --box-log jitter A/B CSV.
            smoother_.update( dets_raw_, dets_ );
            smoother_.log_frame( i, dets_raw_, dets_ );
            // Throttled report: every 15th frame, up to 8 boxes per report.
            if ( dets_.n > 0 && i % 15 == 0 )
            {
                std::cout << "frame " << i << ": " << dets_.n << " det(s)\n";
                for ( int k = 0; k < dets_.n && k < 8; ++k )
                {
                    const Detection& d = dets_.d[k];
                    std::cout << "  cls=" << kCocoNames[d.cls] << " conf=" << std::fixed << std::setprecision( 2 ) << d.conf << " box=("
                              << d.x << ',' << d.y << ' ' << d.w << 'x' << d.h << ")\n";
                }
            }
            if ( preview_ || enc_ )
            {
                // Draw after run(): RGA overwrites the band next frame, so
                // boxes never leak into inference. Cal dumps are also clean
                // (they happen before run()).
                // M3.7: boxes are mapped to tensor coordinates ONCE for
                // both drawing paths — a single source of geometry and
                // colors (det_color); the GPU draws them in one call,
                // the CPU fallback rasterizes the same list.
                const float s = static_cast<float>( model_w_ ) / cfg_.cam_w;
                const float lb_top = static_cast<float>( lg.top );
                boxes_.clear();
                for ( int k = 0; k < dets_.n; ++k )
                {
                    const Detection& d = dets_.d[k];
                    GpuBox b{};
                    b.x0 = d.x * s;
                    b.y0 = d.y * s + lb_top;
                    b.x1 = ( d.x + d.w ) * s;
                    b.y1 = ( d.y + d.h ) * s + lb_top;
                    if ( b.x1 - b.x0 < 2.0f || b.y1 - b.y0 < 2.0f )
                        continue;
                    det_color( d.cls, b.r, b.g, b.b );
                    b.a = 255;
                    boxes_.push( b );
                }
                int fence = -1;  // GPU draw fence, consumed by enc publish
                if ( drawer_ )
                {
                    drawer_->draw( boxes_.d.data(), boxes_.n );
                    // GPU writes: the NUT preview reads the tensor via CPU
                    // and needs a hard sync_cpu(); the h264 path waits for
                    // the exported draw fence inside the encoder (poll),
                    // because this RGA driver ignores acquire fences.
                    // fence < 0 -> sync_cpu fallback (no fence extension).
                    fence = drawer_->export_fence();
                    if ( preview_ || fence < 0 )
                    {
                        drawer_->sync_cpu();
                        if ( fence >= 0 )
                            ::close( fence );
                        fence = -1;
                    }
                }
                else
                {
                    // Fallback: CPU drawing. The RGA imrectangle path is
                    // gone for good: RGA fill on RGB888 is not routable on
                    // RK3588 (RGA3 cores cannot fill; RGA2E policy rejects
                    // rgb888 — dmesg "invalid function policy").
                    // --cpu-draw forces this path even when the GPU drawer
                    // is up (A/B baseline).
                    draw_boxes_cpu( static_cast<uint8_t*>( npus_[0].input_data() ),
                            model_w_,
                            model_h_,
                            model_w_ * 3,
                            boxes_.d.data(),
                            boxes_.n );
                }
                // M3.5: NetPreview publishes the 640x640 model tensor
                // (NUT path). NetEncoder now publishes the same tensor:
                // the GPU/CPU drawing above has landed and is synced, so
                // the RGA band crop carries the boxes into the stream.
                if ( preview_ )
                {
                    preview_->publish( static_cast<const uint8_t*>( npus_[0].input_data() ) );
                }
                if ( enc_ )
                {
                    // Tensor path: the encoder crops the visible band out
                    // of the tensor (boxes already drawn and synced) and
                    // converts it to NV12 1:1 — no upscale.
                    const auto t5 = clk::now();
                    enc_->publish( npus_[0].input_fd(), fence,
                            std::chrono::duration_cast<std::chrono::microseconds>( t0 - t_start ).count() );
                    const auto t6 = clk::now();
                    // publish() is synchronous in the pipeline thread:
                    // fence-poll + RGA crop + encode + mux + TCP write.
                    // Zero without a viewer (publish early-returns).
                    enc_ms = ms( t5, t6 );
                }
            }
            ++ok;
        }
        FrameTrace tr;
        tr.cam = ms( t0, t1 );
        tr.dec = ms( t1, t2 );
        tr.rga = ms( t2, t3 );
        tr.infer = ms( t3b, t4 );
        tr.enc = enc_ms;
        g_t.add( tr );
    }

    if ( stop_.load() )
        std::cout << "stop: signal received, draining and flushing...\n";

    // M4: drain the ring — publish whatever the workers already finished.
    while ( npool_ > 1 && pub_n_ > 0 )
        ok += publish_oldest();

    const double wall = std::chrono::duration<double>( clk::now() - t_start ).count();
    if ( reconnects > 0 )
    {
        std::cout << "camera disconnects recovered: " << reconnects << '\n';
    }
    g_t.report( "TOTAL " );
    std::cout << "Processed " << ok << " frames in " << std::fixed << std::setprecision( 3 ) << wall << " s -> "
              << ( wall > 0 ? ok / wall : 0.0 ) << " FPS\n";
    return ok;
}


// --- M4: context-pool machinery --------------------------------------------

Pipeline::~Pipeline()
{
    quit_.store( true );
    for ( auto& sl : slots_ )
    {
        std::lock_guard<std::mutex> lk( sl.mx );
        sl.cv.notify_all();
    }
    for ( auto& t : workers_ )
        if ( t.joinable() )
            t.join();
}

void Pipeline::worker_loop( int slot )
{
    Slot& sl = slots_[slot];
    for ( ;; )
    {
        {
            std::unique_lock<std::mutex> lk( sl.mx );
            sl.cv.wait( lk, [this, slot] {
                return quit_.load() || slots_[slot].st == Slot::QUEUED;
            } );
            if ( quit_.load() )
                return;
        }
        // Infer WITHOUT holding the slot mutex: the claim scan on the main
        // thread must not block on a running rknn_run. Plain writes below
        // are published to the main thread by the lock acquire/release of
        // the DONE transition.
        const auto ti0 = clk::now();
        const bool rga_ok = sl.rga_ok;
        bool okr = false;
        if ( rga_ok )
        {
            okr = ( sl.eng->run() == 0 );
            sl.tr.infer = ms( ti0, clk::now() );
            if ( okr )
            {
                yolo_postprocess( *sl.eng, static_cast<float>( cfg_.cam_w ), static_cast<float>( cfg_.cam_h ),
                        static_cast<float>( model_w_ ), static_cast<float>( model_h_ ), sl.lb_top, sl.lb_h, sl.dets, cfg_.conf_thr,
                        cfg_.iou_thr );
            }
        }
        else
        {
            sl.tr.infer = 0.0;  // legacy: run() is short-circuited on a failed RGA
        }
        sl.npu_ok = rga_ok && okr;
        {
            std::lock_guard<std::mutex> lk( sl.mx );
            sl.st = Slot::DONE;
            // Publish may have given up waiting (bounded wait): free
            // the slot here -- it is already out of the publish FIFO.
            if ( sl.drop )
            {
                sl.st = Slot::FREE;
                sl.frame = -1;
                sl.drop = false;
            }
        }
        sl.cv.notify_all();
    }
}

void Pipeline::dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, int lb_top, int lb_h, double cam_ms, double dec_ms, double rga_ms )
{
    Slot& sl = slots_[slot];
    {
        std::lock_guard<std::mutex> lk( sl.mx );
        sl.frame = frame_idx;
        sl.ts_us = ts_us;
        sl.rga_ok = rga_ok;
        sl.npu_ok = false;
        sl.drop = false;
        sl.tr.cam = cam_ms;
        sl.tr.dec = dec_ms;
        sl.tr.rga = rga_ms;
        sl.lb_top = lb_top;
        sl.lb_h = lb_h;
        sl.st = Slot::QUEUED;
    }
    pub_slots_[( pub_head_ + pub_n_ ) % 3] = slot;
    ++pub_n_;
    sl.cv.notify_one();
}

int Pipeline::publish_oldest()
{
    const int ps = pub_slots_[pub_head_];
    Slot& sl = slots_[ps];
    std::unique_lock<std::mutex> lk( sl.mx );
    // Bounded wait (P0-2): rknn_run normally always completes; a
    // timeout means a wedged NPU/runtime -- drop the frame instead of
    // hanging the whole pipeline (and Ctrl-C) forever. quit_ releases
    // the wait on shutdown as well. The slot is NOT freed here: the
    // worker may still be mid-infer on this tensor, handing it back
    // to RGA would overwrite memory the NPU is reading. The worker
    // sees Slot::drop at its DONE transition and frees the slot.
    const bool done = sl.cv.wait_for( lk, std::chrono::seconds( 5 ), [this, &sl] {
        return sl.st == Slot::DONE || quit_.load();
    } );
    if ( !done || sl.st != Slot::DONE )
    {
        if ( sl.st == Slot::QUEUED )
            sl.drop = true;
        std::cerr << "pipeline: publish wait gave up on slot " << ps << " (st="
                  << static_cast<int>( sl.st ) << "), frame " << sl.frame << " dropped\n";
        lk.unlock();
        pub_head_ = ( pub_head_ + 1 ) % 3;
        --pub_n_;
        return 0;
    }

    double enc_ms = 0.0;
    if ( sl.npu_ok )
    {
        // M3.10: EMA smoothing on scene-coord boxes; the raw list is kept
        // only for the --box-log jitter A/B CSV.
        smoother_.update( sl.dets, dets_ );
        smoother_.log_frame( sl.frame, sl.dets, dets_ );
        // Throttled report: every 15th frame, up to 8 boxes per report.
        if ( dets_.n > 0 && sl.frame % 15 == 0 )
        {
            std::cout << "frame " << sl.frame << ": " << dets_.n << " det(s)\n";
            for ( int k = 0; k < dets_.n && k < 8; ++k )
            {
                const Detection& d = dets_.d[k];
                std::cout << "  cls=" << kCocoNames[d.cls] << " conf=" << std::fixed << std::setprecision( 2 ) << d.conf << " box=("
                          << d.x << ',' << d.y << ' ' << d.w << 'x' << d.h << ")\n";
            }
        }
        if ( preview_ || enc_ )
        {
            // Boxes mapped to tensor coordinates ONCE for both drawing
            // paths — the same geometry as the legacy block (det_color).
            const float s = static_cast<float>( model_w_ ) / cfg_.cam_w;
            const float lb_top = sl.lb_top;
            boxes_.clear();
            for ( int k = 0; k < dets_.n; ++k )
            {
                const Detection& d = dets_.d[k];
                GpuBox b{};
                b.x0 = d.x * s;
                b.y0 = d.y * s + lb_top;
                b.x1 = ( d.x + d.w ) * s;
                b.y1 = ( d.y + d.h ) * s + lb_top;
                if ( b.x1 - b.x0 < 2.0f || b.y1 - b.y0 < 2.0f )
                    continue;
                det_color( d.cls, b.r, b.g, b.b );
                b.a = 255;
                boxes_.push( b );
            }
            int fence = -1;  // GPU draw fence, consumed by enc publish
            if ( drawer_ )
            {
                drawer_->draw_slot( ps, boxes_.d.data(), boxes_.n );
                fence = drawer_->export_fence();
                if ( preview_ || fence < 0 )
                {
                    drawer_->sync_cpu();
                    if ( fence >= 0 )
                        ::close( fence );
                    fence = -1;
                }
            }
            else
            {
                draw_boxes_cpu( static_cast<uint8_t*>( sl.eng->input_data() ), model_w_, model_h_, model_w_ * 3, boxes_.d.data(),
                        boxes_.n );
            }
            if ( preview_ )
            {
                preview_->publish( static_cast<const uint8_t*>( sl.eng->input_data() ) );
            }
            if ( enc_ )
            {
                const auto t5 = clk::now();
                enc_->publish( sl.eng->input_fd(), fence, sl.ts_us );
                const auto t6 = clk::now();
                enc_ms = ms( t5, t6 );
            }
        }
    }
    sl.tr.enc = enc_ms;
    g_t.add( sl.tr );
    sl.st = Slot::FREE;
    sl.frame = -1;
    lk.unlock();
    pub_head_ = ( pub_head_ + 1 ) % 3;
    --pub_n_;
    return sl.npu_ok ? 1 : 0;
}

void Pipeline::cal_dump_maybe( RknnEngine& e, int frame_idx )
{
    // Calibration dump: P6 PPM of the model input. The only diagnostic
    // CPU pixel read. --dump-cal writes a single /tmp/cal.ppm; --cal N
    // saves up to N frames (one every --cal-interval) into dataset/.
    const bool cal_wanted = cfg_.dump_cal || ( cfg_.cal_frames > 0 && cal_saved_ < cfg_.cal_frames );
    const bool cal_due = !cal_done_ || ( frame_idx - last_cal_ >= cfg_.cal_interval );
    if ( !cal_wanted || !cal_due )
        return;
    const size_t bytes = static_cast<size_t>( model_w_ ) * model_h_ * 3;
    std::string path = "/tmp/cal.ppm";
    if ( !cfg_.dump_cal )
    {
        std::ostringstream name;
        // The series name follows the FRAME counter, as documented
        // (README / tools/oracle_check.py protocol): with --cal-interval K
        // the files are frames 0, K, 2K, .... cal_saved_ stays as the
        // --cal N cap.
        // Series directory: --cal-dir overrides the default dataset/.
        name << ( cfg_.cal_dir.empty() ? std::string( "dataset/cal_" )
                                       : cfg_.cal_dir + "/cal_" )
             << std::setw( 4 ) << std::setfill( '0' ) << frame_idx << ".ppm";
        path = name.str();
    }
    std::ofstream out( path, std::ios::binary );
    if ( out && e.input_size() >= bytes )
    {
        out << "P6\n" << model_w_ << ' ' << model_h_ << "\n255\n";
        out.write( reinterpret_cast<const char*>( e.input_data() ), static_cast<std::streamsize>( bytes ) );
        if ( cfg_.dump_cal )
        {
            std::cout << "calibration dump: " << path << " (" << model_w_ << 'x' << model_h_ << ")\n";
        }
        ++cal_saved_;
        last_cal_ = frame_idx;
        cal_done_ = true;
    }
    else
    {
        std::cerr << "calibration dump failed\n";
        cal_done_ = true;  // do not retry-spam on a broken sink
    }
}

void Pipeline::pool_discard()
{
    while ( pub_n_ > 0 )
    {
        Slot& sl = slots_[pub_slots_[pub_head_]];
        std::unique_lock<std::mutex> lk( sl.mx );
        // The worker may still be mid-infer on this slot: wait for DONE
        // (rknn_run always completes; quit_ is not set here), then free
        // the slot without publishing.
        sl.cv.wait( lk, [&sl] {
            return sl.st != Slot::QUEUED;
        } );
        sl.st = Slot::FREE;
        sl.frame = -1;
        lk.unlock();
        pub_head_ = ( pub_head_ + 1 ) % 3;
        --pub_n_;
    }
}
