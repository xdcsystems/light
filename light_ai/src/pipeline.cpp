#include <cstring>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <sys/stat.h>  // stat(): startup node fallback
#include <unistd.h>

#include "pipeline.hpp"
#include "yolo_postprocess.hpp"
#include "dma_sync.hpp"
#include "box_mapper.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    inline double ms( clk::time_point a, clk::time_point b ) { 
        return std::chrono::duration<double, std::milli>( b - a ).count(); 
    }

    // M3.7 CPU baseline: 1-px outlines straight into the NPU tensor (RGB888).
    // The caller pre-maps boxes to tensor coordinates — the same geometry as the GPU path;
    // colors come from the box itself. Used when the GPU drawer
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
            const int x1 = std::max( x0, clamp( static_cast<int>( b.x1 - 1.0f ), w - 1 ) );
            const int y1 = std::max( y0, clamp( static_cast<int>( b.y1 - 1.0f ), h - 1 ) );
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
  : cfg_( cfg )
  , cam_( cfg_.cam_w, cfg_.cam_h )
  , dec_( cfg_.cam_w, cfg_.cam_h )
{
    // M4: the pool is a startup-time decision; npus_ never grows after
    // this loop, so the slot pointers stay valid for the whole session.
    // Only two modes exist: ring of one and the 3-engine pool. The CLI
    // accepts 0|1|3; anything below 3 falls back to ring of one so a
    // direct call can never create the untested 2-engine mode.

    npool_ = cfg_.pool >= 3 ? 3 : 1;    
    npus_.reserve( npool_ );

    for ( int k = 0; k < npool_; ++k )
        npus_.emplace_back( cfg_.model.c_str() );

    // Source bring-up (identity, setup, variant selection): the net
    // branch made the ctor cross the readability-function-size limit.
    init_source();

    for ( auto& e : npus_ )
    {
        if ( e.setup_io() != 0 )
            throw std::runtime_error( "rknn setup_io failed" );
    }

    setup_cores();

    // Single source of truth for the model geometry: the model file itself.
    // NHWC layout: dims = {N, H, W, C}, so W is dims[n-2] and H is dims[n-3].
    const rknn_tensor_attr& ia = npus_[0].input_attr();  // all engines share the model geometry
    if ( ia.n_dims < 4 )
    {
        std::cerr << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
        throw std::runtime_error( "rknn: input tensor is not 4-D NHWC" );
    }

    // Gray letterbox bars on every pooled tensor: RGA writes only the
    // visible band (drect), so the 128-fill done here survives every frame.
    for ( auto& e : npus_ )
        std::memset( e.input_data(), 128, e.input_size() );

    model_w_ = static_cast<int>( ia.dims[ia.n_dims - 2] );
    model_h_ = static_cast<int>( ia.dims[ia.n_dims - 3] );
    cal_dumper_.init( cfg_.dump_cal, cfg_.cal_frames, cfg_.cal_interval, cfg_.cal_dir, model_w_, model_h_ );

    // Letterbox geometry computed once via band_geom:
    // the same numbers the RGA crop writes.
    lb_ = band_geom( cfg_.cam_w, cfg_.cam_h, model_w_, model_h_ );

    init_net_sink( ia );
    init_gpu_drawer();
    init_aux();

    if ( cfg_.cal_frames > 0 )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
    }

    // M4: bind slots to engines, then start the worker threads — the unified
    // path routes every frame through the slot ring; with pool 1 the single
    // worker is woken by dispatch and publish waits on it.
    for ( int k = 0; k < npool_; ++k )
        slots_[k].eng = &npus_[k];

    // No throwing steps below this line: once workers_ are started, the
    // dtor takes over shutdown (quit_ -> notify -> join) and the ctor
    // must not leave the object half-initialized after them.
    for ( int k = 0; k < npool_; ++k )
        workers_.emplace_back( [this, k] { worker_loop( k ); } );

    with_src( []( auto& s ) { s.start(); } );  // no-op for the net source
}

// Source bring-up: source identity (cam_node_ vs the URL), the full
// setup for the active branch and the static variant selection.
// Throwing: a failed connect here is fatal, same as a failed camera
// setup (run() never sees a half-open source).

void Pipeline::init_source()
{
    cam_node_ = cfg_.device;

    if ( use_net_src() )
    {
        // Network MJPEG source: the camera stays closed, no V4L2 node.
        // The source contract is identical, so the recovery ladder in
        // run() works unchanged; a failed connect here is fatal, same
        // as a failed camera setup.

        net_src_.emplace( cfg_.cam_w, cfg_.cam_h );

        // The URL is this source's identity: the recovery ladder
        // passes cam_node_ to recover(), so it must hold the URL,
        // not the (empty in net mode) --device value. A URL never
        // re-enumerates, so recover() keeps new_id == id.

        cam_node_ = cfg_.net_src;
        std::string why;
        if ( net_src_->setup_device( cfg_.net_src, &why ) != 0 )
            throw std::runtime_error( "net-src: " + why );
    }
    else
    {
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
    }

    // Static source selection (project rules: no virtuals):
    // the active source is a pointer variant over the member objects. 
    // Set once here, before any use; M5 adds alternatives without touching call sites.

    if ( use_net_src() )
        src_ = &*net_src_;
    else
        src_ = &cam_;
}

// M2.5/M4: core pinning.
// Pool 1 keeps cfg_.core verbatim (AUTO by default — the A/B baseline is untouched).
// The pool pins its own: one engine per core, via the per-core flags 1/2/4.
void Pipeline::setup_cores()
{
    if ( npool_ == 1 )
    {
        if ( cfg_.core != 0 )
        {
            if ( npus_[0].set_core_mask( static_cast<unsigned>( cfg_.core ) ) != 0 )
                throw std::runtime_error( "rknn set_core_mask failed" );
            std::cout << "npu: core mask 0x" << std::hex << cfg_.core << std::dec << "\n";
        }
        return;
    }

    constexpr int cores[] = { 1, 2, 4 };
    if ( npool_ > static_cast<int>( std::size( cores ) ) )
        throw std::runtime_error( "npu pool size > 3 is not supported" );

    for ( int k = 0; k < npool_; ++k )
    {
        // clang 11 analyzer cannot fold std::size() into a constant and misses the
        // guard above; reading cores[k] is safe because k < npool_ <= std::size(cores).
        // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage)
        if ( npus_[k].set_core_mask( static_cast<unsigned>( cores[k] ) ) != 0 )
            throw std::runtime_error( "rknn set_core_mask failed" );
    }
    std::cout << "npu: pool of " << npool_ << " engines, one per core\n";
    t_.set_pool( npool_ );
}

// Preview/encoder bring-up (M3.5). Verbatim move out of the ctor.
void Pipeline::init_net_sink( const rknn_tensor_attr& ia )
{
    if ( !cfg_.net )
        return;

    // Hard confirmation of tensor contiguity:
    // if size_with_stride equals w*h*3, the buffer is exactly the PPM dump layout (stride = w*3).
    std::cout << "net: tensor stride check: size_with_stride=" << ia.size_with_stride
              << " vs " << model_w_ * model_h_ * 3 << "\n";

    if ( cfg_.net_h264 )
    {
        // M3.5 pull: empty net_host = TCP pull server
        // (listener on net_port, the viewer connects to the board's LAN IP);
        // a non-empty net_host keeps the legacy UDP push to that destination.
        // The h264 encoder consumes the NPU input tensor AFTER box drawing,
        // at the NATIVE letterbox-band size (no upscale):
        // w = model_w, h = cam_h * (model_w / cam_w) — for the default
        // 1280x720 camera and a 640x640 tensor that is 640x360 (16:9).
        // Band geometry comes from the pipeline's single LbGeom:
        // the same numbers the RGA crop writes.

        // Band-crop target: landscape crops a horizontal strip of the
        // full tensor width; portrait a vertical strip of the full tensor height.
        // The stream width is 16-aligned up (MPP hor_stride requirement); the tail stays gray.

        enc_.emplace( model_w_, model_h_, lb_, cfg_.net_fps, cfg_.net_port, cfg_.net_host, cfg_.net_bitrate );
        if ( cfg_.net_host.empty() )
            std::cout << "net: h264 PULL server on port " << cfg_.net_port << ", " << cfg_.net_bitrate
                      << " kbps (viewer: ffplay tcp://<board-ip>:" << cfg_.net_port << ")\n";
        else
            std::cout << "net: h264 preview -> " << cfg_.net_host << ':' << cfg_.net_port
                      << ", " << cfg_.net_bitrate << " kbps\n";
    }
    else
    {
        preview_.emplace( model_w_, model_h_, model_w_ * 3, cfg_.net_fps, cfg_.net_port );
        std::cout << "net: preview server on port " << cfg_.net_port << "\n";
    }
}

// M3.7: GPU box drawing (Mali G610, EGL/GBM) straight into the NPU tensor.
// Init once, only with a preview sink active. On failure the CPU path
// (draw_boxes_cpu, this file) stays as the fallback.
// --cpu-draw forces the CPU path (A/B baseline).
void Pipeline::init_gpu_drawer()
{
    if ( ( !preview_ && !enc_ ) || cfg_.cpu_draw )
        return;

    drawer_.emplace();

    // M4: one EGLImage per slot, attached once at startup (attach_slot is idempotent per slot);
    // draw switches FBOs between slots.
    bool drew_ok = drawer_->init();
    for ( int k = 0; drew_ok && k < npool_; ++k )
        drew_ok = drawer_->attach_slot( k, npus_[k].input_fd(), model_w_, model_h_,
                static_cast<uint32_t>( model_w_ ) * 3 );

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
// 1280x720 -> 224 is 5.7x — single pass, aux not allocated, no DRM pool split.
// 1920x1080 still needs the 960x540 intermediate.
void Pipeline::init_aux()
{
    if ( cfg_.cam_w > 8 * model_w_ || cfg_.cam_h > 8 * model_h_ )
    {
        aux_fd_ = dec_.aux_fd( static_cast<size_t>( cfg_.cam_w / 2 ) * ( cfg_.cam_h / 2 ) * 2 );
        if ( aux_fd_ < 0 )
            throw std::runtime_error( "pipeline: aux buffer alloc failed" );
        return;
    }
    std::cout << "aux not needed: " << cfg_.cam_w << 'x' << cfg_.cam_h
              << " fits single-pass into " << model_w_ << 'x' << model_h_ << '\n';
}

int Pipeline::run()
{
    int ok = 0;
    const auto t_start = clk::now();

    smoother_.set_log( cfg_.box_log );
    for ( int i = 0; i < cfg_.frames && !stop_.load(); ++i )
    {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        int cam_err = 0;
        
        const int idx = with_src( [&]( auto& s ) { return s.dequeue( &sz, &cam_err ); } );
        if ( idx < 0 )
        {
            if ( handle_source_failure( cam_err ) != 0 )
                return -1;
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
            fd_src = with_src( [&]( auto& s ) {
                return dec_.decode( static_cast<const uint8_t*>( s.buffer( idx ) ), sz );
            } );
        }
        else
        {
            std::cerr << "camera: zero-size frame skipped\n";
        }
        
        with_src( [&]( auto& s ) { s.requeue( idx ); } );

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
        if ( warmup_left_ > 0 )
        {
            --warmup_left_;
            if ( warmup_left_ == 0 )
                std::cout << "camera: warm-up done, publishing resumed\n";
            continue;
        }

        // ---- unified path: every frame goes through the slot ring ----
        // pool 1 == ring of one: dispatch, then publish immediately —
        // the pipeline thread blocks on the worker's DONE, exactly the
        // legacy synchronous semantics. pool 3 keeps publish-before-reuse.
        const double cam_ms = ms( t0, t1 );
        const double dec_ms = ms( t1, t2 );
        if ( pub_.n == npool_ )
            ok += publish_oldest();
        const int slot = acquire_slot( ok );
        if ( slot < 0 )
            break;  // stop requested with every slot in flight: leave through the normal path
        const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t0 - t_start ).count();
        frame_dispatch( slot, i, fd_src, ts_us, cam_ms, dec_ms );
        if ( npool_ == 1 )
            ok += publish_oldest();
    }

    if ( stop_.load() )
        std::cout << "stop: signal received, draining and flushing...\n";

    // M4: drain the ring — publish whatever the workers already finished.
    while ( pub_.n > 0 )
        ok += publish_oldest();

    const double wall = std::chrono::duration<double>( clk::now() - t_start ).count();
    t_.report( "TOTAL " );
    rep_.summary( ok, wall, reconnects_ );
    return ok;
}

// Disconnect policy: ENODEV/EIO mean the source is gone (unplug, USB
// hiccup, Wi-Fi drop) — worth timed recovery attempts; any other errno
// is a hard error and ends the run with exit code 1 (run() returns -1,
// main() maps it to 1). The net source reports only these two classes
// by contract, so the ladder below is source-agnostic: the recovery
// POLICY (attempts, pacing, warmup, pool discard) stays here, the
// MECHANISM of one step lives in the source's recover(). A recovery
// attempt consumes one frame of the --frames budget and contributes
// nothing to the timing stats. 20 attempts: observed USB re-enumeration
// on this board can take longer than the original 10x1s window allowed.

int Pipeline::handle_source_failure( int cam_err )
{
    constexpr int k_cam_reconnect_max = 20;
    constexpr auto k_cam_reconnect_pause = std::chrono::seconds( 1 );
    constexpr int k_warmup_frames = 3;

    const bool recoverable = ( cam_err == ENODEV || cam_err == EIO );
    std::cerr << "source: dequeue failed: " << strerror( cam_err )
              << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
    if ( !recoverable )
        return -1;
    for ( int a = 1; a <= k_cam_reconnect_max; ++a )
    {
        // Stop outranks recovery: SIGINT/SIGTERM must end the run
        // within one attempt, not after the full 20-attempt ladder.
        if ( stop_.load() )
        {
            std::cerr << "source: stop requested, recovery aborted\n";
            return -1;
        }
        std::this_thread::sleep_for( k_cam_reconnect_pause );
        std::string new_id;
        std::string why;
        if ( with_src( [&]( auto& s ) { return s.recover( cam_node_, new_id, &why ); } ) == 0 )
        {
            if ( new_id != cam_node_ )
            {
                std::cout << "source: identity adopted: " << cam_node_ << " -> " << new_id << '\n';
                cam_node_ = new_id;
            }
            ++reconnects_;
            warmup_left_ = k_warmup_frames;
            std::cout << "source: reconnected on attempt " << a << '\n';

            // M4: frames from the dead stream are never published —
            // drop the pending slots (their infer may still be in
            // flight; wait for DONE, then free without publishing).

            if ( npool_ > 1 )
                pool_discard();
            return 0;
        }
        std::cerr << "source: reconnect attempt failed: " << why << '\n';
    }
    std::cerr << "source: reconnect failed after " << k_cam_reconnect_max << " attempts, giving up\n";
    return -1;
}

// Claim-or-drain: a FREE slot is NOT guaranteed here. On the 5 s
// publish timeout the dropped slot stays in flight until its own
// worker finishes the in-flight rknn_run — only that worker may
// free it (freeing here would race the NPU tensor read). So: scan
// for FREE; if every slot is in flight, drain the publish queue
// when it has entries, otherwise sleep briefly and rescan (a
// rknn_run away from finishing, ~30 ms). The stop_ check keeps
// Ctrl-C from parking here forever on a wedged NPU. Returns the
// claimed slot index, or -1 when stop was requested with every
// slot in flight.

int Pipeline::acquire_slot( int& ok )
{
    bool starved_warned = false;
    while ( true )
    {
        for ( int k = 0; k < npool_; ++k )
        {
            std::lock_guard<std::mutex> lk( slots_[k].mx );
            if ( slots_[k].st == Slot::FREE )
                return k;
        }
        if ( stop_.load() )
            return -1;
        if ( !starved_warned )
        {
            std::cerr << "pipeline: all " << npool_ << " slots in flight, waiting for a worker to release one\n";
            starved_warned = true;
        }
        if ( pub_.n > 0 )
            ok += publish_oldest();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

// RGA resize into the slot tensor, optional cal dump, one-time
// cross-check of the RGA letterbox geometry against the single-owner
// one, then dispatch. Verbatim move out of run().

void Pipeline::frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms )
{
    Slot& sl = slots_[slot];
    const auto t3 = clk::now();

    // Stale-line drop before the RGA rewrite: see cache_drop().
    // Full tensor: draws and preview reads from previous frames
    // may have left resident lines anywhere in it.

    dma::cache_drop( sl.eng->input_data(),
            static_cast<size_t>( model_w_ ) * model_h_ * 3 );
    LbGeom lg {};
    const bool rga_ok =
            rga_.resize( fd_src, cfg_.cam_w, cfg_.cam_h, sl.eng->input_fd(), model_w_, model_h_, aux_fd_, cfg_.cam_w / 2, cfg_.cam_h / 2, &lg,
                    dec_.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1 );
    cal_dumper_.maybe( frame_idx, static_cast<const uint8_t*>( sl.eng->input_data() ), sl.eng->input_size() );
    const auto t3b = clk::now();
    // Cross-check RGA geometry against the single-owner one (warn once).
    if ( !lb_warned_ && ( lg.left != lb_.left || lg.top != lb_.top || lg.w != lb_.w || lg.h != lb_.h ) )
    {
        lb_warned_ = true;
        std::cerr << "pipeline: RGA letterbox drift: x=" << lg.left << " top=" << lg.top << " w=" << lg.w << " h=" << lg.h
                  << " vs x=" << lb_.left << " top=" << lb_.top << " w=" << lb_.w << " h=" << lb_.h << std::endl;
    }
    dispatch( slot, frame_idx, ts_us, rga_ok, lb_, cam_ms, dec_ms, ms( t3, t3b ) );
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
    while ( !quit_.load() )  // the only exit is the quit check after the wait
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
                const auto tp0 = clk::now();
                yolo_postprocess( *sl.eng, static_cast<float>( cfg_.cam_w ), static_cast<float>( cfg_.cam_h ),
                        static_cast<float>( model_w_ ), static_cast<float>( model_h_ ), sl.lb,
                        sl.dets, cfg_.conf_thr, cfg_.iou_thr );
                sl.tr.post = ms( tp0, clk::now() );
            }
        }
        else
        {
            sl.tr.infer = 0.0;  // legacy: run() is short-circuited on a failed RGA
            sl.tr.post = 0.0;
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

void Pipeline::dispatch( int slot, int frame_idx, int64_t ts_us, bool rga_ok, const LbGeom& lb, double cam_ms, double dec_ms, double rga_ms )
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
        sl.lb = lb;
        sl.st = Slot::QUEUED;
    }
    pub_.push( slot );
    sl.cv.notify_one();
}

int Pipeline::publish_oldest()
{
    const int ps = pub_.front();
    Slot& sl = slots_[ps];
    std::unique_lock<std::mutex> lk( sl.mx );

    // Bounded wait (P0-2): 
    // rknn_run normally always completes; 
    // a timeout means a wedged NPU/runtime -- drop the frame instead of
    // hanging the whole pipeline (and Ctrl-C) forever. 
    // quit_ releases the wait on shutdown as well.
    // The slot is NOT freed here: 
    // the worker may still be mid-infer on this tensor, handing it back
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
        pub_.pop();
        return 0;
    }

    double enc_ms = 0.0;
    if ( sl.npu_ok )
    {
        // M3.10: EMA smoothing on scene-coord boxes; the raw list is kept
        // only for the --box-log jitter A/B CSV.

        smoother_.update( sl.dets, dets_ );
        smoother_.log_frame( sl.frame, sl.dets, dets_ );
        tracker_.update( dets_, sl.ts_us, trajs_ );
        
        rep_.report( sl.frame, dets_, trajs_ );
        if ( preview_ || enc_ )
            enc_ms = draw_and_publish( sl, ps );
    }

    sl.tr.enc = enc_ms;
    t_.add( sl.tr );
    const bool npu_ok = sl.npu_ok;  // read under the lock: after unlock
                                    // the worker may already be refilling
    sl.st = Slot::FREE;
    sl.frame = -1;
    lk.unlock();
    pub_.pop();
    return npu_ok ? 1 : 0;
}

// Viewer gating (rule: output only for a connected viewer,
// recognition never gates). Both flags are per-frame atomics:
// a late joiner misses at most one frame of boxes. With no
// viewer the GPU draw pass is skipped entirely - the encoder
// gate would drop the frame anyway, this just saves the draw.

double Pipeline::draw_and_publish( Slot& sl, int ps )
{
    double enc_ms = 0.0;
    const bool enc_live = enc_ && enc_->viewer_active();
    const bool prev_live = preview_ && preview_->viewer_connected();
    if ( !enc_live && !prev_live )
        return enc_ms;

    // Boxes mapped to tensor coordinates ONCE for both drawing paths.
    // Inverse of the postprocess mapping: det = scene * (lb / cam) + origin.

    const float sx = sl.lb.w / static_cast<float>( cfg_.cam_w );
    const float sy = sl.lb.h / static_cast<float>( cfg_.cam_h );
    const float lb_left = sl.lb.left;
    const float lb_top = sl.lb.top;
    boxes_.clear();
    fill_gpu_boxes( dets_, trajs_, tracker_, boxes_, model_w_, model_h_, sx, sy, lb_left, lb_top );
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
        dma::DmaSync wr( sl.eng->input_fd(), true );
        dma::cache_drop( sl.eng->input_data(), static_cast<size_t>( model_w_ ) * model_h_ * 3 );
        draw_boxes_cpu( static_cast<uint8_t*>( sl.eng->input_data() ),
                        model_w_, model_h_, model_w_ * 3, boxes_.d.data(), boxes_.n );
        dma::cache_clean( sl.eng->input_data(), static_cast<size_t>( model_w_ ) * model_h_ * 3 );
    }

    // READ bracket covers the NUT preview (CPU read) only;
    // enc_->publish() uses RGA hardware DMA and must run outside CPU sync.
    if ( prev_live )
    {
        dma::DmaSync rd( sl.eng->input_fd(), false );
        preview_->publish( static_cast<const uint8_t*>( sl.eng->input_data() ) );
    }
    if ( enc_live )
    {
        const auto t5 = clk::now();
        enc_->publish( sl.eng->input_fd(), fence, sl.ts_us );
        const auto t6 = clk::now();
        enc_ms = ms( t5, t6 );
    }
    return enc_ms;
}

void Pipeline::pool_discard()
{
    while ( pub_.n > 0 )
    {
        Slot& sl = slots_[pub_.front()];
        std::unique_lock<std::mutex> lk( sl.mx );

        // Bounded wait (same 5 s cap as the publish wait, P0-2 class of
        // risk): rknn_run normally completes. quit_ in the predicate
        // means a wedged NPU during source recovery can no longer block
        // shutdown -- Ctrl-C releases this wait just like publish_oldest.
        // On timeout or quit the slot is NOT freed here: the NPU may
        // still be reading its tensor. It is marked drop instead, so the
        // worker frees it at its DONE transition -- the same contract
        // publish_oldest() uses on its own timeout path.
        
        sl.cv.wait_for( lk, std::chrono::seconds( 5 ), [this, &sl] {
            return quit_.load() || sl.st != Slot::QUEUED;
        } );
        if ( sl.st == Slot::QUEUED )
        {
            if ( !sl.drop )
                sl.drop = true;  // worker frees at DONE
        }
        else
        {
            sl.st = Slot::FREE;
            sl.frame = -1;
        }
        lk.unlock();
        pub_.pop();
    }
}
