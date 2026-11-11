#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>  // stat(): startup node fallback
#include <thread>

#include "dma_sync.hpp"
#include "pipeline.hpp"
#include "timings.hpp"
#include "yolo_postprocess.hpp"

namespace
{
    using clk = std::chrono::steady_clock;

    // M5: per-source box-log path: with several sources "boxes.csv"
    // becomes "boxes_s1.csv" (suffix before the extension; no dot -> append).
    std::string source_box_log( const std::string& path, int idx, int nsrc )
    {
        if ( path.empty() || nsrc <= 1 )
            return path;
        const size_t dot = path.rfind( '.' );
        const std::string suf = "_s" + std::to_string( idx );
        return dot == std::string::npos ? path + suf : path.substr( 0, dot ) + suf + path.substr( dot );
    }

}  // namespace

Pipeline::Pipeline( const Config& cfg )
  : cfg_( cfg )
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

    // M5: source bring-up (identity, setup, per-source decoders) --
    // each SourceSlot owns its MppDecoder (its own resolution and
    // detected 4:2:0/4:2:2 format). Runs BEFORE the model dims are read:
    // the per-source letterbox pass below needs model_w_/model_h_.
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
    {
        dma::DmaSync wr( e.input_fd(), true );
        std::memset( e.input_data(), 128, e.input_size() );
        dma::cache_clean( e.input_data(), e.input_size() );
    }

    model_w_ = static_cast<int>( ia.dims[ia.n_dims - 2] );
    model_h_ = static_cast<int>( ia.dims[ia.n_dims - 3] );
    cal_dumper_.init( cfg_.dump_cal, cfg_.cal_frames, cfg_.cal_interval, cfg_.cal_dir, model_w_, model_h_ );

    // M5: per-source letterbox geometry (the same numbers the RGA crop
    // writes); src_w_/src_h_ become the max across sources.
    src_w_ = 0;
    src_h_ = 0;
    for ( int s = 0; s < nsrc_; ++s )
    {
        // absent at startup: zero geometry, and band_geom divides by
        // it (sdiv-by-0 returns 0 on aarch64 -- silently). probe_source
        // computes the lb on the real attach.
        if ( srcs_[s].w == 0 || srcs_[s].h == 0 )
            continue;
        srcs_[s].lb = band_geom( srcs_[s].w, srcs_[s].h, model_w_, model_h_ );
        if ( srcs_[s].w > src_w_ )
            src_w_ = srcs_[s].w;
        if ( srcs_[s].h > src_h_ )
            src_h_ = srcs_[s].h;
    }

    // Canonical letterbox band: 4:3 of the model input. The canvas is
    // source-independent now -- absent slots show the NO SIGNAL idle
    // plate (idle_tick), real bands land per-frame via prep_frame's
    // band_geom (probe refresh still fills per-slot geometry).
    const LbGeom enc_lb = band_geom( model_w_, model_h_ * 3 / 4, model_w_, model_h_ );
    sink_.init( cfg_, npus_, model_w_, model_h_, enc_lb, nsrc_ );
    init_aux();

    if ( cfg_.cal_frames > 0 && cfg_.cal_dir.empty() )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
    }

    // bind slots to engines, then start the worker threads — the unified
    // path routes every frame through the slot ring; with pool 1 the single
    // worker is woken by dispatch and publish waits on it.
    for ( int k = 0; k < npool_; ++k )
        slots_[k].eng = &npus_[k];

    // No throwing steps below this line: once workers_ are started, the
    // dtor takes over shutdown (quit_ -> notify -> join) and the ctor
    // must not leave the object half-initialized after them.
    // The source start below used to violate this: it throws on a failed STREAMON,
    // and unwinding past started workers_ would destroy the joinable
    // threads (std::terminate).
    // Guarded: the exception is caught, the source retries the full reconnect
    // path, and nothing propagates past this point.
    for ( int k = 0; k < npool_; ++k )
        workers_.emplace_back( [this, k] { worker_loop( k ); } );

    for ( int s = 0; s < nsrc_; ++s )
        start_source( s );
}

void Pipeline::start_source( int idx )
{
    if ( srcs_[idx].absent )
        return;
    SourceSlot& sl = srcs_[idx];
    bool src_started = false;
    try
    {
        sl.src.start();  // no-op for the net source
        src_started = true;
    }
    catch ( const std::exception& e )
    {
        std::cerr << "pipeline: source start failed: " << e.what() << '\n';
    }
    catch ( ... )
    {
        std::cerr << "pipeline: source start failed: unknown exception\n";
    }
    if ( !src_started )
    {
        constexpr int k_start_retries = 5;
        for ( int a = 1; a <= k_start_retries; ++a )
        {
            std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
            std::string new_id;
            std::string why;
            if ( sl.src.recover( sl.node, new_id, &why ) == 0 )
            {
                if ( new_id != sl.node )
                {
                    std::cout << "source: identity adopted: " << sl.node << " -> " << new_id << '\n';
                    sl.node = new_id;
                }
                constexpr int k_warmup_frames = 3;
                sl.warmup_left = k_warmup_frames;
                src_started = true;
                std::cout << "pipeline: source started on recovery attempt " << a << '\n';
                break;
            }
            std::cerr << "pipeline: source start recovery attempt " << a << '/' << k_start_retries
                      << " failed: " << why << '\n';
        }
    }
    if ( !src_started )
        std::cerr << "pipeline: source never started - run() recovery ladder takes over\n";
}

void Pipeline::init_one_source( int idx, const SourceEndpoint& ep )
{
    SourceSlot& sl = srcs_[idx];
    sl.node = ep.uri;
    sl.kind = ep.kind;
    if ( ep.kind == SourceKind::NetMjpeg )
    {
        sl.src.use_net();
        std::string why;
        if ( sl.src.setup_device( sl.node, &why ) != 0 )
            { sl.absent = true; std::cerr << "net-src: absent at startup: " << why << "\n"; return; }
    }
    else
    {
        sl.src.use_camera();
        auto& cam = sl.src.camera();  // init-path only: USB node discovery
        if ( !sl.node.empty() )
        {
            struct stat st {};
            if ( stat( sl.node.c_str(), &st ) != 0 )
            {
                std::cerr << "camera: configured node " << sl.node << " not present, falling back to USB discovery\n";
                std::string found;
                if ( find_usb_capture_node( cam, sl.node, found, &scan_log_ms_ ) != 0 )
                    { sl.absent = true; std::cerr << "camera: node " << sl.node << " absent at startup\n"; return; }
                std::cout << "camera: discovery adopted " << found << '\n';
                sl.node = found;
            }
        }
        std::string cam_why;
        if ( cam.setup_device( sl.node, &cam_why ) != 0 )
            { sl.absent = true; std::cerr << "camera: absent at startup: " << cam_why << "\n"; return; }
    }
    sl.w = sl.src.width();
    sl.h = sl.src.height();
    // M5: per-source decoder init -- sits after the if/else by design,
    // so BOTH the net and the camera branch get it.
    if ( sl.dec.init( sl.w, sl.h ) != 0 )
        throw std::runtime_error( "mpp decoder init failed for " + sl.node );
    std::cout << "source: geometry " << sl.w << 'x' << sl.h << '\n';
}


// Hot-attach probe (one step): (re)bring-up of one absent slot. Never
// throws: any failure keeps the slot absent, the next probe retries.
// Two paths: never-attached (w/h == 0) goes setup_device + start like
// at init; detached keeps the decoder alive and re-runs recover() --
// the same one-step mechanism the old inline ladder used, now off the
// frame loop. MppDecoder::init self-cleans on failure, so a decoder
// retry is safe; a setup_device failure self-teardowns (README).
void Pipeline::probe_source( int idx )
{
    SourceSlot& sl = srcs_[idx];
    const bool first_attach = ( sl.w == 0 || sl.h == 0 );
    if ( first_attach && sl.kind == SourceKind::NetMjpeg )
    {
        std::string why;
        if ( sl.src.setup_device( sl.node, &why ) != 0 )
            return;  // still absent: connect refused / stream not ready yet
    }
    else if ( first_attach )
    {
        auto& cam = sl.src.camera();  // init-path only: USB node discovery
        if ( !sl.node.empty() )
        {
            std::string found;
            if ( find_usb_capture_node( cam, sl.node, found, &scan_log_ms_ ) != 0 )
                return;
            if ( !found.empty() && found != sl.node )
            {
                std::cout << "camera: hot-attach adopted " << sl.node << " -> " << found << '\n';
                sl.node = found;
            }
        }
        std::string cam_why;
        if ( cam.setup_device( sl.node, &cam_why ) != 0 )
            return;
    }
    if ( !first_attach )
    {
        // detached: recover() re-runs the full open+verify path for the
        // camera (STREAMON included) and reconnects for the net source;
        // the decoder and its buffers stay alive across the recovery.
        std::string new_id;
        std::string why;
        if ( sl.src.recover( sl.node, new_id, &why ) != 0 )
            return;  // still absent: device gone / stream not ready
        if ( new_id != sl.node )
        {
            std::cout << "source s" << idx << ": identity adopted: " << sl.node
                      << " -> " << new_id << '\n';
            sl.node = new_id;
        }
    }
    sl.w = sl.src.width();
    sl.h = sl.src.height();
    // refresh what init_source() derived from the geometry while the slot
    // was absent (letterbox, max sizes, aux). absent flips last, so no
    // concurrent dispatcher sees a half-updated slot.
    sl.lb = band_geom( sl.w, sl.h, model_w_, model_h_ );
    if ( sl.w > src_w_ )
        src_w_ = sl.w;
    if ( sl.h > src_h_ )
        src_h_ = sl.h;
    if ( ( sl.w > 8 * model_w_ || sl.h > 8 * model_h_ ) && sl.aux_fd < 0 )
    {
        sl.aux_fd = sl.dec.aux_fd( static_cast<size_t>( sl.w / 2 ) * ( sl.h / 2 ) * 2 );
        if ( sl.aux_fd < 0 )
        {
            std::cerr << "probe s" << idx << ": aux buffer alloc failed - staying absent\n";
            return;
        }
    }
    if ( first_attach && sl.dec.init( sl.w, sl.h ) != 0 )
    {
        std::cerr << "probe s" << idx << ": mpp decoder init failed - staying absent\n";
        return;
    }
    if ( first_attach )
    {
        try
        {
            sl.src.start();  // STREAMON for the camera, no-op for the net source
        }
        catch ( const std::exception& e )
        {
            std::cerr << "probe s" << idx << ": start failed: " << e.what() << '\n';
            return;
        }
        catch ( ... )
        {
            std::cerr << "probe s" << idx << ": start failed: unknown exception\n";
            return;
        }
    }
    constexpr int k_warmup_frames = 3;
    sl.warmup_left = k_warmup_frames;
    sl.last_ok_tp = clk::now();
    sl.absent.store( false );
    std::cout << "source s" << idx << ( first_attach ? ": attached (hot) - geometry "
                                                     : ": re-attached - geometry " )
              << sl.w << 'x' << sl.h << '\n';
}

// The prober thread: polls absent slots on a 2 s cadence. One thread for
// all sources: a net probe may block for the connect timeout (5 s) -- it
// must not stall the live path, hence a dedicated thread instead of
// probing from the frame loop. V4L2 attach latency <= ~2 s, net <= ~7 s.
void Pipeline::probe_loop()
{
    while ( !probe_quit_.load() )
    {
        ++probe_ticks_;
        for ( int s = 0; s < nsrc_; ++s )
        {
            if ( probe_quit_.load() || !srcs_[s].absent.load() )
                continue;
            // visibility: the 1st and every 15th attempt (~30 s) show
            // the prober is alive; the camera scan prints on its own.
            if ( probe_ticks_ == 1 || probe_ticks_ % 15 == 0 )
                std::cout << "probe: s" << s << " absent, attempt " << probe_ticks_ << '\n';
            probe_source( s );
        }
        for ( int k = 0; k < 20 && !probe_quit_.load(); ++k )
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
}

void Pipeline::init_source()
{
    std::vector<SourceEndpoint> eps = cfg_.sources;
    if ( eps.empty() )
    {
        if ( !cfg_.net_src.empty() )
            eps.push_back( { SourceKind::NetMjpeg, cfg_.net_src } );
        else if ( !cfg_.device.empty() )
            eps.push_back( { SourceKind::V4L2, cfg_.device } );
    }
    if ( eps.size() > static_cast<size_t>( kMaxSources ) )
        throw std::runtime_error( "pipeline: at most 3 sources are supported (one per NPU core)" );

    nsrc_ = static_cast<int>( eps.size() );
    // an empty source list is a legitimate waiting-for-attach run (README);
    // the sleep loop itself lands with the udev patch
    if ( nsrc_ == 0 )
        std::cout << "sources configured: none - waiting for attach\n";
    // M5: per-source decoders -- mixed resolutions (1280x960 USB next to
    // 1280x720 net) are now supported; the shared-resolution gate is gone.
    for ( int s = 0; s < nsrc_; ++s )
        init_one_source( s, eps[s] );
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

// Aux buffer only for the two-pass path (source over 8x the model input).
// 1280x720 -> 224 is 5.7x — single pass, aux not allocated, no DRM pool split.
// 1920x1080 still needs the 960x540 intermediate.
void Pipeline::init_aux()
{
    // M5: aux is per-source (lives in the owning decoder's DRM pool):
    // one source may need the two-pass path while others do not.
    bool any_aux = false;
    for ( int s = 0; s < nsrc_; ++s )
    {
        SourceSlot& ss = srcs_[s];
        if ( ss.w <= 8 * model_w_ && ss.h <= 8 * model_h_ )
            continue;
        ss.aux_fd = ss.dec.aux_fd( static_cast<size_t>( ss.w / 2 ) * ( ss.h / 2 ) * 2 );
        if ( ss.aux_fd < 0 )
            throw std::runtime_error( "pipeline: aux buffer alloc failed for source " + ss.node );
        any_aux = true;
    }
    if ( !any_aux )
        std::cout << "aux not needed: max " << src_w_ << 'x' << src_h_
                  << " fits single-pass into " << model_w_ << 'x' << model_h_ << '\n';
}

// NO SIGNAL idle publish: while at least one configured source is
// absent, absent tiles show the gray NO SIGNAL plate in the stream.
// Paced like --net-fps (25 FPS in follow-source mode); idle encodes
// are NOT counted in --frames or the timing stats and never touch
// the NPU. A fully live composition skips the work entirely.
void Pipeline::idle_tick()
{
    if ( !sink_.enabled() )
        return;
    const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0_ ).count();
    const int64_t step_us = cfg_.net_fps > 0 ? 1000000LL / cfg_.net_fps : 40000;
    if ( last_idle_us_ >= 0 && now_us - last_idle_us_ < step_us )
        return;
    bool absent[ kMaxSources ] = {};
    bool any_absent = nsrc_ == 0;  // no configured sources: one gray canvas
    for ( int s = 0; s < nsrc_; ++s )
    {
        absent[ s ] = srcs_[ s ].absent.load();
        any_absent = any_absent || absent[ s ];
    }
    if ( !any_absent )
        return;
    last_idle_us_ = now_us;
    sink_.publish_idle( now_us, absent, nsrc_ );
}

int Pipeline::try_dequeue_round( int wait_ms, uint32_t& sz, int& cam_err )
{
    constexpr double k_stall_ms = 2000.0;
    idle_tick();  // mixed state: absent tiles keep publishing NO SIGNAL
    for ( int k = 0; k < nsrc_; ++k )
    {
        const int s = ( cur_src_ + 1 + k ) % nsrc_;
        if ( srcs_[s].absent )
            continue;
        const int idx = srcs_[s].src.dequeue( &sz, &cam_err, wait_ms );
        if ( idx >= 0 )
        {
            cur_src_ = s;
            srcs_[s].last_ok_tp = clk::now();
            return idx;
        }
        if ( cam_err != EAGAIN && cam_err != EINTR )
        {
            cur_src_ = s;
            return -1;
        }
        if ( ms( srcs_[s].last_ok_tp, clk::now() ) >= k_stall_ms )
        {
            std::cerr << "source s" << s << ": no frame in " << static_cast<int>( k_stall_ms )
                      << " ms - treating as recoverable stall\n";
            cur_src_ = s;
            cam_err = EIO;
            return -1;
        }
    }
    return -2;
}

bool Pipeline::front_slot_done()
{
    if ( pub_.empty() )
        return false;
    Slot& sl = slots_[pub_.front()];
    std::lock_guard<std::mutex> lk( sl.mx );
    return sl.st == Slot::DONE;
}

void Pipeline::drain_done_slots( int& ok )
{
    while ( front_slot_done() && !stop_.load() )
        ok += publish_oldest();
}

int Pipeline::dequeue_next_source( uint32_t& sz, int& cam_err, int& ok )
{
    drain_done_slots( ok );
    idle_tick();
    if ( nsrc_ == 0 )
    {
        // waiting for attach: nothing to dequeue, park until stop
        // (hot-adding sources beyond the configured set is a separate feature)
        while ( !stop_.load() )
        {
            idle_tick();
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
        }
        cam_err = EINTR;
        return -1;
    }
    if ( nsrc_ <= 1 && pub_.empty() )
    {
        cur_src_ = 0;
        int r = -1;
        while ( !stop_.load() )
        {
            if ( srcs_[0].absent.load() )
            {
                // all-absent park: NO SIGNAL idle publish + 40 ms quanta;
                // probe_loop() flips the flag
                idle_tick();
                std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
                continue;
            }
            r = srcs_[0].src.dequeue( &sz, &cam_err );
            if ( r >= 0 || ( cam_err != EAGAIN && cam_err != EINTR ) )
                break;
        }
        return r;
    }
    while ( !stop_.load() )
    {
        drain_done_slots( ok );
        bool any_live = false;
        for ( int s = 0; s < nsrc_; ++s )
            any_live = any_live || !srcs_[s].absent.load();
        if ( !any_live )
        {
            // all-absent park: NO SIGNAL idle publish + 40 ms quanta;
            // probe_loop() flips the flags
            idle_tick();
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
            continue;
        }
        int r = try_dequeue_round( 0, sz, cam_err );
        if ( r != -2 )
            return r;
        drain_done_slots( ok );
        r = try_dequeue_round( 1, sz, cam_err );
        if ( r != -2 )
            return r;
    }
    cam_err = EINTR;
    return -1;
}

int Pipeline::run()
{
    int ok = 0;
    const auto t_start = clk::now();
    t0_ = std::chrono::steady_clock::now();  // idle pts base: same timeline as live ts_us
    // hot-attach prober (README): armed for the whole run(); the local
    // guard joins it on ANY exit -- drain, fatal error, exception unwind
    struct ProbeStop
    {
        Pipeline* p;
        ~ProbeStop()
        {
            p->probe_quit_.store( true );
            if ( p->probe_th_.joinable() )
                p->probe_th_.join();
        }
    } probe_stop { this };
    if ( nsrc_ > 0 )
        probe_th_ = std::thread( &Pipeline::probe_loop, this );

    for ( int s = 0; s < nsrc_; ++s )
    {
        srcs_[s].smoother.set_log( source_box_log( cfg_.box_log, s, nsrc_ ) );
        srcs_[s].last_ok_tp = t_start;
    }
    // frames == 0 (the default): no frame budget -- run until stop
    // (Ctrl-C); the signal path below drains and reports as usual.
    for ( int i = 0; ( cfg_.frames <= 0 || i < cfg_.frames ) && !stop_.load(); )
    {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        int cam_err = 0;

        const int idx = dequeue_next_source( sz, cam_err, ok );
        if ( idx < 0 )
        {
            if ( stop_.load() )
                break;
            if ( handle_source_failure( cam_err ) != 0 )
                return -1;
            const auto now = clk::now();
            for ( int s = 0; s < nsrc_; ++s )
                srcs_[s].last_ok_tp = now;
            continue;
        }
        SourceSlot& active_src = srcs_[cur_src_];
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
            fd_src = active_src.dec.decode(
                    static_cast<const uint8_t*>( active_src.src.buffer( idx ) ), sz );
        }
        else
        {
            std::cerr << "camera: zero-size frame skipped\n";
        }
        
        active_src.src.requeue( idx );

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
        if ( active_src.warmup_left > 0 )
        {
            --active_src.warmup_left;
            if ( active_src.warmup_left == 0 )
                std::cout << "camera: warm-up done, publishing resumed\n";
            continue;
        }

        // ---- unified path: every frame goes through the slot ring ----
        // pool 1 == ring of one: dispatch, then publish immediately —
        // the pipeline thread blocks on the worker's DONE, exactly the
        // legacy synchronous semantics. pool 3 keeps publish-before-reuse.
        const double cam_ms = ms( t0, t1 );
        const double dec_ms = ms( t1, t2 );
        drain_done_slots( ok );
        if ( pub_.size() == npool_ )
            ok += publish_oldest();
        const int slot = acquire_slot( ok );
        if ( slot < 0 )
            break;  // stop requested with every slot in flight: leave through the normal path
        const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t1 - t_start ).count();
        frame_dispatch( slot, active_src.frames++, fd_src, ts_us, cam_ms, dec_ms );
        ++i;
        if ( npool_ == 1 )
            ok += publish_oldest();
    }

    if ( stop_.load() )
        std::cout << "stop: signal received, draining and flushing...\n";

    // M4: drain the ring — publish whatever the workers already finished.
    while ( !pub_.empty() )
        ok += publish_oldest();

    const double wall = std::chrono::duration<double>( clk::now() - t_start ).count();
    t_.report( "TOTAL " );
    rep_.summary( ok, wall, reconnects_ );
    return ok;
}

// Disconnect policy: ENODEV/EIO mean the source is gone (unplug, USB
// hiccup, Wi-Fi drop) - the slot flips to absent and probe_loop() owns
// the re-attach on its 2 s cadence (recover() for a detached slot,
// setup_device/start for one that never attached). Any other errno is
// a hard error and ends the run with exit code 1 (run() returns -1,
// main() maps it to 1). The net source reports only these two classes
// by contract. The recovery MECHANISM is one recover() step inside
// the source; the POLICY (cadence) is the prober's. The old inline
// 20x1 s ladder is gone: it froze the frame loop and starved any
// other live source for up to 20 s.

int Pipeline::handle_source_failure( int cam_err )
{
    const bool recoverable = ( cam_err == ENODEV || cam_err == EIO );
    std::cerr << "source: dequeue failed: " << strerror( cam_err )
              << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
    if ( !recoverable )
        return -1;
    // M4: frames from the dead stream are never published - drop the
    // pending slots (their infer may still be in flight; wait for
    // DONE, then free without publishing).
    if ( npool_ > 1 )
        pool_discard( cur_src_ );
    ++reconnects_;
    srcs_[cur_src_].absent.store( true );
    srcs_[cur_src_].warmup_left = 0;
    std::cout << "source s" << cur_src_ << ": detached - probe_loop will re-attach\n";
    return 0;
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
        if ( !pub_.empty() )
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

    const SourceSlot& ss = srcs_[cur_src_];
    if ( sl.lb.w > 0
         && ( sl.lb.left != ss.lb.left || sl.lb.top != ss.lb.top || sl.lb.w != ss.lb.w || sl.lb.h != ss.lb.h ) )
    {
        dma::DmaSync wr( sl.eng->input_fd(), true );
        std::memset( sl.eng->input_data(), 128, sl.eng->input_size() );
    }
    dma::cache_drop( sl.eng->input_data(), sl.eng->input_size() );
    LbGeom lg {};
    const bool rga_ok =
            rga_.resize( fd_src, ss.w, ss.h, sl.eng->input_fd(), model_w_, model_h_, ss.aux_fd, ss.w / 2, ss.h / 2, &lg,
                    ss.dec.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1 );
    cal_dumper_.maybe( frame_idx, sl.eng->input_fd(), static_cast<const uint8_t*>( sl.eng->input_data() ), sl.eng->input_size() );
    const auto t3b = clk::now();
    // Cross-check RGA geometry against the owning source's (warn once).
    if ( !lb_warned_ && ( lg.left != ss.lb.left || lg.top != ss.lb.top || lg.w != ss.lb.w || lg.h != ss.lb.h ) )
    {
        lb_warned_ = true;
        std::cerr << "pipeline: RGA letterbox drift (src " << cur_src_ << "): x=" << lg.left << " top=" << lg.top
                  << " w=" << lg.w << " h=" << lg.h
                  << " vs x=" << ss.lb.left << " top=" << ss.lb.top << " w=" << ss.lb.w << " h=" << ss.lb.h << '\n';
    }
    dispatch( slot, frame_idx, ts_us, rga_ok, ss.lb, cam_ms, dec_ms, ms( t3, t3b ) );
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
                yolo_postprocess( *sl.eng, static_cast<float>( sl.src_w ), static_cast<float>( sl.src_h ),
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
    const SourceSlot& ss = srcs_[cur_src_];
    Slot& sl = slots_[slot];
    {
        std::lock_guard<std::mutex> lk( sl.mx );
        sl.src_idx = cur_src_;
        sl.src_w = ss.w;
        sl.src_h = ss.h;
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

        SourceSlot& ss = srcs_[sl.src_idx];
        ss.smoother.update( sl.dets, dets_ );
        ss.smoother.log_frame( sl.frame, sl.dets, dets_ );
        ss.tracker.update( dets_, sl.ts_us, trajs_ );

        rep_.report( sl.frame, dets_, trajs_, sl.src_idx, nsrc_ );
        if ( sink_.enabled() )
            enc_ms = sink_.draw_and_publish( sl, ps, dets_, trajs_, ss.tracker );
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

void Pipeline::pool_discard( int src_idx )
{
    const int n = pub_.size();
    for ( int i = 0; i < n; ++i )
    {
        const int s = pub_.front();
        pub_.pop();
        Slot& sl = slots_[s];
        std::unique_lock<std::mutex> lk( sl.mx );
        if ( sl.src_idx != src_idx )
        {
            lk.unlock();
            pub_.push( s );
            continue;
        }

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
    }
}
