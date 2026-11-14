#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>  // stat(): startup node fallback
#include <thread>

#include "core/frame_view.hpp"
#include "core/log.hpp"
#include "cpu_affinity.hpp"
#include "dma_sync.hpp"
#include "inference_pool.hpp"
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

Pipeline::Pipeline( const Config& cfg, const StopFlag& stop, const Log& log )
  : cfg_( cfg )
  , pool_( cfg_.infer, YoloDecode( cfg_.infer.conf_thr, cfg_.infer.iou_thr ), log, PoolOptions {},
          cfg_.infer.model.c_str() )
  , t_( log )
  , sink_( log )
  , stop_( stop )
  , log_( log )
{
    // pool_ is up by now: the engines are loaded, their I/O is set up,
    // they are pinned to NPU cores and the workers wait for frames. If
    // anything below throws, pool_'s destructor joins the workers -- the
    // ctor can no longer leave joinable threads behind.
    t_.set_pool( pool_.size() );

    // M5: source bring-up (identity, setup, per-source decoders) --
    // each SourceSlot owns its MppDecoder (its own resolution and
    // detected 4:2:0/4:2:2 format). Runs BEFORE the per-source
    // letterbox pass below, which needs the source geometry.
    init_source();

    // Single source of truth for the model geometry: the model file itself
    // (RknnEngine::input_width/height). All engines share the model.
    const rknn_tensor_attr& ia = pool_.engine( 0 ).input_attr();
    if ( ia.n_dims < 4 )
    {
        log_.warn() << "pipeline: unexpected input tensor ndims=" << ia.n_dims << '\n';
        throw std::runtime_error( "rknn: input tensor is not 4-D NHWC" );
    }

    // Gray letterbox bars on every pooled tensor: RGA writes only the
    // visible band (drect), so the 128-fill done here survives every frame.
    for ( int k = 0; k < pool_.size(); ++k )
    {
        const TensorRef tn = pool_.tensor( k );
        dma::fill_and_flush( tn.fd, tn.data, 128, tn.size );
    }

    model_w_ = pool_.engine( 0 ).input_width();
    model_h_ = pool_.engine( 0 ).input_height();
    cal_dumper_.init( cfg_.cal, model_w_, model_h_ );

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
    sink_.init( cfg_.stream, pool_.tensors(), model_w_, model_h_, enc_lb, nsrc_ );
    init_aux();

    if ( cfg_.cal.frames > 0 && cfg_.cal.dir.empty() )
    {
        std::error_code ec;
        std::filesystem::create_directories( "dataset", ec );
        if ( ec )
            throw std::runtime_error( "pipeline: cannot create dataset/: " + ec.message() );
    }

    // start_source() never throws: a failed STREAMON is caught and retried,
    // and the run() recovery ladder takes over if the retries run out.
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
        log_.warn() << "pipeline: source start failed: " << e.what() << '\n';
    }
    catch ( ... )
    {
        log_.warn() << "pipeline: source start failed: unknown exception\n";
    }
    if ( !src_started )
    {
        constexpr int k_start_retries = 5;
        for ( int a = 1; a <= k_start_retries; ++a )
        {
            std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
            std::string new_id;
            std::string why;
            if ( sl.src.recover( sl.node, new_id, busy_v4l2_nodes( idx ), &why ) == 0 )
            {
                if ( new_id != sl.node )
                {
                    log_.info() << "source: identity adopted: " << sl.node << " -> " << new_id << '\n';
                    sl.node = new_id;
                }
                constexpr int k_warmup_frames = 3;
                sl.warmup_left = k_warmup_frames;
                src_started = true;
                log_.info() << "pipeline: source started on recovery attempt " << a << '\n';
                break;
            }
            log_.warn() << "pipeline: source start recovery attempt " << a << '/' << k_start_retries
                        << " failed: " << why << '\n';
        }
    }
    if ( !src_started )
        log_.warn() << "pipeline: source never started - run() recovery ladder takes over\n";
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
            { sl.absent = true; log_.warn() << "net-src: absent at startup: " << why << "\n"; return; }
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
                log_.warn() << "camera: configured node " << sl.node << " not present, falling back to USB discovery\n";
                std::string found;
                if ( find_usb_capture_node( cam, sl.node, found, busy_v4l2_nodes( idx ), &scan_log_ms_ ) != 0 )
                    { sl.absent = true; log_.warn() << "camera: node " << sl.node << " absent at startup\n"; return; }
                log_.info() << "camera: discovery adopted " << found << '\n';
                sl.node = found;
            }
        }
        std::string cam_why;
        if ( cam.setup_device( sl.node, &cam_why ) != 0 )
            { sl.absent = true; log_.warn() << "camera: absent at startup: " << cam_why << "\n"; return; }
    }
    sl.w = sl.src.width();
    sl.h = sl.src.height();
    // M5: per-source decoder init -- sits after the if/else by design,
    // so BOTH the net and the camera branch get it.
    if ( sl.dec.init( sl.w, sl.h ) != 0 )
        throw std::runtime_error( "mpp decoder init failed for " + sl.node );
    log_.info() << "source: geometry " << sl.w << 'x' << sl.h << '\n';
}

// Busy-node reservation (audit: multi-camera node hijacking): the /dev
// nodes of the OTHER V4L2 slots, live or absent alike. A detached slot
// still owns its node -- probe_loop() re-attaches it on its own cadence,
// so a scanning slot must never trial-open or adopt that node: uvcvideo
// accepts the trial while the node is momentarily free, and two slots
// would then compete for one physical camera.
std::vector<std::string> Pipeline::busy_v4l2_nodes( int self_idx ) const
{
    std::vector<std::string> busy;
    for ( int k = 0; k < nsrc_; ++k )
    {
        if ( k == self_idx || srcs_[k].kind != SourceKind::V4L2 || srcs_[k].node.empty() )
            continue;
        busy.push_back( srcs_[k].node );
    }
    return busy;
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
            if ( find_usb_capture_node( cam, sl.node, found, busy_v4l2_nodes( idx ), &scan_log_ms_ ) != 0 )
                return;
            if ( !found.empty() && found != sl.node )
            {
                log_.info() << "camera: hot-attach adopted " << sl.node << " -> " << found << '\n';
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
        if ( sl.src.recover( sl.node, new_id, busy_v4l2_nodes( idx ), &why ) != 0 )
            return;  // still absent: device gone / stream not ready
        if ( new_id != sl.node )
        {
            log_.info() << "source s" << idx << ": identity adopted: " << sl.node
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
            // w/h are already non-zero here: keep them and the next probe
            // takes the detached recover() path, which never runs dec.init
            // -- the slot would come back live with an uninitialized decoder.
            // Roll back so the retry is a clean first_attach (dec branch below).
            sl.w = 0;
            sl.h = 0;
            log_.warn() << "probe s" << idx << ": aux buffer alloc failed - staying absent\n";
            return;
        }
    }
    if ( first_attach && sl.dec.init( sl.w, sl.h ) != 0 )
    {
        // init() failed AFTER sl.w/sl.h were written: without the reset the
        // next probe computes first_attach == false and takes the detached
        // recover() path, which never calls dec.init. A successful recover()
        // (the camera itself is fine) then flips absent to false and frames
        // flow into a never-initialized decoder. Reset keeps the retry on
        // the first_attach path; dec.init self-cleans on failure, so retry
        // is safe. lb/src_w_/src_h_ need no rollback: unused while absent,
        // and the src maxes are monotonic diagnostics.
        sl.w = 0;
        sl.h = 0;
        log_.warn() << "probe s" << idx << ": mpp decoder init failed - staying absent\n";
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
            log_.warn() << "probe s" << idx << ": start failed: " << e.what() << '\n';
            return;
        }
        catch ( ... )
        {
            log_.warn() << "probe s" << idx << ": start failed: unknown exception\n";
            return;
        }
    }
    constexpr int k_warmup_frames = 3;
    sl.warmup_left = k_warmup_frames;
    // Stamp BEFORE absent.store: try_dequeue_round reads the stamp only
    // after absent.load(), which synchronizes-with this store (seq_cst).
    sl.last_ok_tp.store( clk::now() );
    sl.absent.store( false );
    log_.info() << "source s" << idx << ( first_attach ? ": attached (hot) - geometry "
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
                log_.info() << "probe: s" << s << " absent, attempt " << probe_ticks_ << '\n';
            probe_source( s );
        }
        for ( int k = 0; k < 20 && !probe_quit_.load(); ++k )
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
}

void Pipeline::init_source()
{
    const std::vector<SourceEndpoint>& eps = cfg_.sources;
    if ( eps.size() > static_cast<size_t>( kMaxSources ) )
        throw std::runtime_error( "pipeline: at most 3 sources are supported (one per NPU core)" );

    nsrc_ = static_cast<int>( eps.size() );
    // an empty source list is a legitimate waiting-for-attach run (README);
    // the sleep loop itself lands with the udev patch
    if ( nsrc_ == 0 )
        log_.info() << "sources configured: none - waiting for attach\n";
    // M5: per-source decoders -- mixed resolutions (1280x960 USB next to
    // 1280x720 net) are now supported; the shared-resolution gate is gone.
    for ( int s = 0; s < nsrc_; ++s )
        init_one_source( s, eps[s] );
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
        log_.info() << "aux not needed: max " << src_w_ << 'x' << src_h_
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
    const int64_t step_us = cfg_.stream.fps > 0 ? 1000000LL / cfg_.stream.fps : 40000;
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
            srcs_[s].last_ok_tp.store( clk::now() );
            return idx;
        }
        if ( cam_err != EAGAIN && cam_err != EINTR )
        {
            cur_src_ = s;
            return -1;
        }
        if ( ms( srcs_[s].last_ok_tp.load(), clk::now() ) >= k_stall_ms )
        {
            log_.warn() << "source s" << s << ": no frame in " << static_cast<int>( k_stall_ms )
                        << " ms - treating as recoverable stall\n";
            cur_src_ = s;
            cam_err = EIO;
            return -1;
        }
    }
    return -2;
}

// Publishes every result that is already finished, in order; never waits.
void Pipeline::drain_done_slots( int& ok )
{
    while ( pool_.front_done() && !stop_.requested() )
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
        while ( !stop_.requested() )
        {
            idle_tick();
            std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
        }
        cam_err = EINTR;
        return -1;
    }
    if ( nsrc_ <= 1 && pool_.empty() )
    {
        cur_src_ = 0;
        int r = -1;
        while ( !stop_.requested() )
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
    while ( !stop_.requested() )
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
    // Spawn order: probe_th_ above was spawned BEFORE this pin, so the
    // prober keeps the scheduler default, as intended (cpu_affinity.hpp).
    pin_current_thread_to_big_cores( "pipeline main", log_ );

    for ( int s = 0; s < nsrc_; ++s )
    {
        srcs_[s].smoother.set_log( source_box_log( cfg_.box_log, s, nsrc_ ) );
        srcs_[s].last_ok_tp.store( t_start );
    }
    // frames == 0 (the default): no frame budget -- run until stop
    // (Ctrl-C); the signal path below drains and reports as usual.
    for ( int i = 0; ( cfg_.frames <= 0 || i < cfg_.frames ) && !stop_.requested(); )
    {
        const auto t0 = clk::now();

        uint32_t sz = 0;
        int cam_err = 0;

        const int idx = dequeue_next_source( sz, cam_err, ok );
        if ( idx < 0 )
        {
            if ( stop_.requested() )
                break;
            if ( handle_source_failure( cam_err ) != 0 )
                return -1;
            const auto now = clk::now();
            for ( int s = 0; s < nsrc_; ++s )
                srcs_[s].last_ok_tp.store( now );
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
            log_.warn() << "camera: zero-size frame skipped\n";
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
                log_.info() << "camera: warm-up done, publishing resumed\n";
            continue;
        }

        // ---- unified path: every frame goes through the InferencePool ----
        // pool 1 == ring of one: dispatch, then publish immediately --
        // the pipeline thread blocks until the worker is done, exactly the
        // legacy synchronous semantics. pool 3 keeps publish-before-reuse:
        // with the FIFO full, the oldest frame is published first.
        const double cam_ms = ms( t0, t1 );
        const double dec_ms = ms( t1, t2 );
        drain_done_slots( ok );
        if ( pool_.full() )
            ok += publish_oldest();
        const int slot = acquire_slot( ok );
        if ( slot < 0 )
            break;  // stop requested with every slot in flight: leave through the normal path
        const int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>( t1 - t_start ).count();
        frame_dispatch( slot, active_src.frames++, fd_src, ts_us, cam_ms, dec_ms );
        ++i;
        if ( pool_.size() == 1 )
            ok += publish_oldest();
    }

    if ( stop_.requested() )
        log_.info() << "stop: signal received, draining and flushing...\n";

    // M4: drain the ring -- publish whatever the workers already finished.
    while ( !pool_.empty() )
        ok += publish_oldest();

    const double wall = std::chrono::duration<double>( clk::now() - t_start ).count();
    t_.report_total();
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
    log_.warn() << "source: dequeue failed: " << strerror( cam_err )
                << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
    if ( !recoverable )
        return -1;
    // M4: frames from the dead stream are never published - drop the
    // pending slots (their infer may still be in flight; the pool waits
    // for it, then frees without publishing). A no-op with one engine:
    // its single frame was published before the next dequeue.
    pool_.discard( cur_src_ );
    ++reconnects_;
    // warmup_left first: once absent is set, the prober may take the slot
    // and write warmup_left itself (probe_source). The seq_cst store below
    // publishes this write to it; the old order was a data race.
    srcs_[cur_src_].warmup_left = 0;
    srcs_[cur_src_].absent.store( true );
    log_.info() << "source s" << cur_src_ << ": detached - probe_loop will re-attach\n";
    return 0;
}

// Claim-or-drain: a Free slot is NOT guaranteed here. After a give-up
// on a wedged NPU the abandoned slot stays in flight until its own
// worker finishes the rknn_run -- only that worker may free it (freeing
// here would race the NPU tensor read; InferSlot). So: ask the pool for
// a Free slot; if every slot is in flight, publish the oldest pending
// result when there is one, otherwise sleep briefly and ask again (a
// rknn_run away from finishing, ~30 ms). The stop_ check keeps Ctrl-C
// from parking here forever on a wedged NPU. Returns the claimed slot
// index, or -1 when stop was requested with every slot in flight.

int Pipeline::acquire_slot( int& ok )
{
    bool starved_warned = false;
    while ( true )
    {
        const int k = pool_.try_acquire();
        if ( k >= 0 )
            return k;
        if ( stop_.requested() )
            return -1;
        if ( !starved_warned )
        {
            log_.warn() << "pipeline: all " << pool_.size() << " slots in flight, waiting for a worker to release one\n";
            starved_warned = true;
        }
        if ( !pool_.empty() )
            ok += publish_oldest();
        else
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
}

// RGA resize into the slot's tensor, optional cal dump, one-time
// cross-check of the RGA letterbox geometry against the single-owner
// one, then the job goes to the pool.

void Pipeline::frame_dispatch( int slot, int frame_idx, int fd_src, int64_t ts_us, double cam_ms, double dec_ms )
{
    const TensorRef tn = pool_.tensor( slot );
    LbGeom& band = tensor_band_[static_cast<size_t>( slot )];
    const auto t3 = clk::now();

    // Stale-line drop before the RGA rewrite: see cache_drop().
    // Full tensor: draws and preview reads from previous frames
    // may have left resident lines anywhere in it.

    // tn.data != nullptr: the InferencePool constructor throws when setup_io()
    // fails, so a live slot always has its tensor mapped; the check states the
    // invariant for the analyzer, like the null guard inside cache_drop().
    const SourceSlot& ss = srcs_[cur_src_];
    if ( tn.data != nullptr && band.w > 0
         && ( band.left != ss.lb.left || band.top != ss.lb.top || band.w != ss.lb.w || band.h != ss.lb.h ) )
    {
        dma::DmaSync wr( tn.fd, true );
        std::memset( tn.data, 128, tn.size );
    }
    dma::cache_drop( tn.data, tn.size );
    LbGeom lg {};
    const bool rga_ok =
            rga_.resize( fd_src, ss.w, ss.h, tn.fd, model_w_, model_h_, ss.aux_fd, ss.w / 2, ss.h / 2, &lg,
                    ss.dec.out_fmt() == MPP_FMT_YUV420SP ? 0 : 1 );
    cal_dumper_.maybe( frame_idx, tn.fd, static_cast<const uint8_t*>( tn.data ), tn.size );
    const auto t3b = clk::now();
    // Cross-check RGA geometry against the owning source's (warn once).
    if ( !lb_warned_ && ( lg.left != ss.lb.left || lg.top != ss.lb.top || lg.w != ss.lb.w || lg.h != ss.lb.h ) )
    {
        lb_warned_ = true;
        log_.warn() << "pipeline: RGA letterbox drift (src " << cur_src_ << "): x=" << lg.left << " top=" << lg.top
                    << " w=" << lg.w << " h=" << lg.h
                    << " vs x=" << ss.lb.left << " top=" << ss.lb.top << " w=" << ss.lb.w << " h=" << ss.lb.h << '\n';
    }

    InferJob job {};
    job.src_idx = cur_src_;
    job.src_w = ss.w;
    job.src_h = ss.h;
    job.frame = frame_idx;
    job.ts_us = ts_us;
    job.rga_ok = rga_ok;
    job.lb = ss.lb;
    job.tr.cam = cam_ms;
    job.tr.dec = dec_ms;
    job.tr.rga = ms( t3, t3b );
    band = ss.lb;
    static_cast<void>( pool_.submit( slot, job ) );  // false: logged by the pool, frame dropped
}

// collect -> analytics -> output. The result is copied out of the pool and
// its slot is Free again before the overlay touches the tensor: that is
// safe because only this thread refills a tensor (frame_dispatch), and it
// does so only after this frame is fully drawn and published.
int Pipeline::publish_oldest()
{
    if ( !pool_.collect( res_ ) )
        return 0;  // gave up on a wedged NPU (logged by the pool): frame dropped

    double enc_ms = 0.0;
    if ( res_.npu_ok )
    {
        // M3.10: EMA smoothing on scene-coord boxes; the raw list is kept
        // only for the --box-log jitter A/B CSV.

        SourceSlot& ss = srcs_[res_.job.src_idx];
        ss.smoother.update( res_.dets, dets_ );
        ss.smoother.log_frame( res_.job.frame, res_.dets, dets_ );
        // Anti-jitter stage 1: the Kalman tracker consumes RAW person
        // detections. Feeding it the EMA-smoothed dets_ double-filtered
        // the signal: the filter velocity lagged the (already damped)
        // positions and the rendered box trailed real motion on every
        // direction change.
        // dets_ stays the BoxSmoother output -- it still feeds the
        // non-person drawing below and the --box-log CSV.
        ss.tracker.update( res_.dets, res_.job.ts_us, trajs_ );

        rep_.report( res_.job.frame, dets_, trajs_, res_.job.src_idx, nsrc_ );
        if ( sink_.enabled() )
        {
            FrameView view {};
            view.tensor_idx = res_.slot;
            view.tensor = pool_.tensor( res_.slot );
            view.lb = res_.job.lb;
            view.src_idx = res_.job.src_idx;
            view.src_w = res_.job.src_w;
            view.src_h = res_.job.src_h;
            view.ts_us = res_.job.ts_us;
            enc_ms = sink_.draw_and_publish( view, dets_, trajs_, ss.tracker );
        }
    }

    res_.job.tr.enc = enc_ms;
    t_.add( res_.job.tr );
    return res_.npu_ok ? 1 : 0;
}
