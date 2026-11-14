#include <algorithm>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstddef>  // offsetof
#include <sstream>
#include <string>
#include <unistd.h>
#include <im2d.h>
#include <rga.h>
#include <libdrm/drm_fourcc.h>  // DRM_FORMAT_*

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/dict.h>
}

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/idle_plate.hpp"
#include "core/log.hpp"
#include "core/param_set_cache.hpp"
#include "core/pts_clock.hpp"
#include "muxer_teardown.hpp"
#include "net_encoder.h"
#include "av_common.hpp"

// Mirror of RKMPPEncContext (ffmpeg-rockchip, libavcodec/rkmppenc.h):
// the rkmpp wrapper keeps the raw MppCtx/MppApi handles in its private
// context. We reach them to force an IDR for a late-joining viewer
// (MPP_ENC_SET_IDR_FRAME) without patching ffmpeg or MPP. Field order
// is pinned by the wrapper AVOptions OFFSET() table, so the asserts
// catch any layout drift at compile time.
struct RKMPPPriv
{
    const AVClass* av_class;
    MppApi* mapi;
    MppCtx mctx;
    AVBufferRef* hwdevice;
    AVBufferRef* hwframe;
    MppEncCfg mcfg;
};
static_assert( offsetof( RKMPPPriv, av_class ) == 0, "rkmpp priv layout drift" );
static_assert( offsetof( RKMPPPriv, mapi ) == 8, "rkmpp priv layout drift" );
static_assert( offsetof( RKMPPPriv, mctx ) == 16, "rkmpp priv layout drift" );
static_assert( offsetof( RKMPPPriv, hwdevice ) == 24, "rkmpp priv layout drift" );
static_assert( offsetof( RKMPPPriv, hwframe ) == 32, "rkmpp priv layout drift" );
static_assert( offsetof( RKMPPPriv, mcfg ) == 40, "rkmpp priv layout drift" );

namespace
{
    void fill_nv12_gray( MppBuffer mb )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
        {
            const size_t sz = mpp_buffer_get_size( mb );
            dma::fill_and_flush( mpp_buffer_get_fd( mb ), p, 128, sz );
        }
    }

    // Fill one canvas cell (NV12 semi-planar): Y rows + interleaved UV
    // rows, bounded to the rect. Multi-view only -- the whole-canvas
    // memset erased every neighbor's tile: live ones went gray until
    // their next publish, an offline one stayed gray until reconnect.
    void fill_nv12_gray_rect( MppBuffer mb, int stride, int vstride, const im_rect& rc )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
        {
            uint8_t* py = static_cast<uint8_t*>( p );
            const size_t ysz = static_cast<size_t>( stride ) * static_cast<size_t>( vstride );
            dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
            for ( int r = 0; r < rc.height; ++r )
                std::memset( py + static_cast<size_t>( rc.y + r ) * stride + rc.x, 128,
                             static_cast<size_t>( rc.width ) );
            uint8_t* puv = py + ysz;
            for ( int r = 0; r < rc.height / 2; ++r )
                std::memset( puv + static_cast<size_t>( rc.y / 2 + r ) * stride + ( rc.x / 2 ) * 2, 128,
                             static_cast<size_t>( rc.width / 2 ) * 2 );
            dma::cache_clean( p, mpp_buffer_get_size( mb ) );
        }
    }

    // NO SIGNAL plate (idle canvas): IdlePlate rasterizes the label into
    // the Y plane once at init (core/idle_plate.hpp); the DMA sync is here.
    void draw_no_signal( MppBuffer mb, const StreamLayout& lay )
    {
        if ( !mb )
            return;
        uint8_t* py = static_cast<uint8_t*>( mpp_buffer_get_ptr( mb ) );
        if ( !py )
            return;
        dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
        IdlePlate plate;
        plate.draw( Plane8 { py, lay.w, lay.vstride }, lay );
        dma::cache_clean( py, mpp_buffer_get_size( mb ) );
    }
}  // namespace

NetEncoder::NetEncoder( const EncoderConfig& cfg, const Log& log )
  : log_( log )
  , cfg_( cfg )
  , pacer_( cfg.layout.nsrc, cfg.fps )
  , pts_( cfg.vfr() )
  , params_( log )
{
    avformat_network_init();

    // The stream geometry (single view: the band at its native size;
    // multi-view: one cell per source) is StreamLayout's, core/encoder_config.hpp.
    const StreamLayout& lay = cfg_.layout;
    if ( lay.nsrc > 1 )
        log_.info() << "enc: multi-view " << lay.nsrc << " sources, tile " << lay.tile_w << 'x' << lay.tile_h
                    << " -> " << lay.w << 'x' << lay.h << '\n';
    else
        log_.info() << "enc: tensor band x=[" << lay.band.left << ',' << lay.band.left + lay.band.w << ") y=["
                    << lay.band.top << ',' << lay.band.top + lay.band.h << ") of " << cfg_.model_w << 'x' << cfg_.model_h
                    << " -> " << lay.w << 'x' << lay.h << '\n';

    // Four NV12 dma-bufs: a plane held by an in-flight encode is never
    // overwritten by RGA. The 3-deep rkmpp task queue can hold a full
    // VFR clump, so the ring stays one slot deeper than the queue.
    // RAII sub-object: a throw below (avcodec_open2, av_frame_alloc)
    // cannot leak the pool -- the sub-object destructs on unwinding.
    // Alloc failures throw from here (fail loud, RknnEngine pattern);
    // the pool has already self-cleaned by then.
    std::string pool_why;
    if ( !pool_.alloc( static_cast<size_t>( lay.w ) * lay.vstride * 3 / 2, MODULE_TAG, __FUNCTION__, &pool_why ) )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: " + pool_why );
    }

    // Idle canvas (NO SIGNAL): a dedicated dma-buf, gray-filled by the
    // pool alloc and stamped with the plate in every tile, once at init.
    // publish_idle() RGA-copies from it -- the NPU tensor is never touched.
    std::string idle_why;
    if ( !idle_pool_.alloc( static_cast<size_t>( lay.w ) * lay.vstride * 3 / 2, MODULE_TAG, __FUNCTION__, &idle_why ) )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: idle canvas: " + idle_why );
    }
    draw_no_signal( idle_pool_.buf[ 0 ], lay );

    const AVCodec* codec = avcodec_find_encoder_by_name( "h264_rkmpp" );
    if ( !codec )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: h264_rkmpp not found in this ffmpeg" );
    }
    auto* ctx = avcodec_alloc_context3( codec );
    if ( !ctx )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: avcodec_alloc_context3 failed" );
    }
    enc_ = ctx;
    ctx->width = lay.w;
    ctx->height = lay.h;
    // DRM_PRIME input: the wrapper takes the frame as a DRM descriptor and
    // imports our dma-buf by fd (rkmpp_submit_frame, DRM_PRIME branch) -
    // zero CPU pixel copies. sw_pix_fmt carries the underlying NV12: the
    // wrapper derives MPP_FMT_YUV420SP from it while pix_fmt is the hw
    // container. With DRM_PRIME the wrapper skips creating its own
    // hwdevice/hwframes pools entirely (encode_init returns early).
    ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    // net-fps 0 = follow the source. The codec gets the native
    // MPEG-TS time base (1/90000) and stamps pts from the real dequeue
    // times (VFR): night ~25, day up to 30 — no fixed ceiling. The GOP
    // stays 25 so a late joiner still waits at most about one second.
    // CFR keeps the legacy 1/fps setup verbatim.
    ctx->time_base = cfg_.vfr() ? AVRational { 1, 90000 } : AVRational { 1, cfg_.fps };
    // VFR: rc still needs a numeric fps — 0/1 reaches MPP as the auto
    // marker (65535) and the CBR per-frame budget collapses (stream
    // dries to ~83 kbps). EncoderConfig::nominal_fps(): 25, 25 per source
    // in multi-view, for rate control only; pts stay VFR from dequeue times.
    ctx->framerate = AVRational { cfg_.nominal_fps(), 1 };
    ctx->bit_rate = cfg_.bit_rate();
    // gop_size reaches MPP as rc:gop (rkmpp_set_enc_cfg):
    // a captured stream shows I-frames every 26 for GOP 25.
    ctx->gop_size = cfg_.gop();  // 1 s: a late joiner waits at most one GOP
    ctx->max_b_frames = 0;  // no reorder: minimal latency
    // Rate control: this ffmpeg-rockchip build has no rc_mode AVOption
    // -- a generic rc_mode option here is silently ignored, and the
    // encoder runs with the MPP default (VBR).
    // The real knob is the wrapper's own MppEncCfg, reachable through its
    // private context (RKMPPPriv above, the same channel as the forced-IDR
    // path); CBR is applied after avcodec_open2 below.
    if ( avcodec_open2( ctx, codec, nullptr ) < 0 )
    {
        avcodec_free_context( &enc_ );
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: avcodec_open2(h264_rkmpp) failed" );
    }
    // Force CBR through MPP: see the rate-control comment above. At this
    // point mpp_init has run and nothing has been encoded yet, so a
    // re-issued MPP_ENC_SET_CFG is safe. Non-fatal on any mismatch.
    RKMPPPriv* rc_priv = static_cast< RKMPPPriv* >( ctx->priv_data );
    if ( rc_priv && rc_priv->mctx && rc_priv->mapi && rc_priv->mcfg )
    {
        mpp_enc_cfg_set_s32( rc_priv->mcfg, "rc:mode", MPP_ENC_RC_MODE_CBR );
        const MPP_RET rc_set = rc_priv->mapi->control( rc_priv->mctx, MPP_ENC_SET_CFG, rc_priv->mcfg );
        if ( rc_set == MPP_OK )
            log_.info() << "enc: rc mode CBR applied via MPP_ENC_SET_CFG\n";
        else
            log_.info() << "enc: rc mode CBR rejected by MPP (rc " << rc_set << "), encoder default stays\n";
    }
    else
    {
        log_.info() << "enc: rkmpp priv unavailable, encoder default rc stays\n";
    }
    log_.info() << "enc: h264_rkmpp open: " << lay.w << 'x' << lay.h << ", " << cfg_.kbps() << " kbps, gop "
                << ctx->gop_size << '\n';
    if ( cfg_.vfr() )
        log_.info() << "enc: VFR follow-source mode: time_base 1/90000, pts from dequeue times\n";

    pkt_ = av_packet_alloc();
    if ( !pkt_ || !init_frames() )
    {
        free_frames();
        av_packet_free( &pkt_ );
        avcodec_free_context( &enc_ );
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: av_frame/av_packet_alloc failed" );
    }

    try
    {
        thread_ = std::thread( &NetEncoder::serve, this );
    }
    catch ( ... )
    {
        free_frames();
        av_packet_free( &pkt_ );
        avcodec_free_context( &enc_ );
        avformat_network_deinit();
        throw;
    }
}

NetEncoder::~NetEncoder()
{
    quit_.store( true );
    {
        std::lock_guard<std::mutex> lk( mx_ );
        cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        pkt_cv_.notify_all();
    }
    if ( thread_.joinable() )
        thread_.join();
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        clear_pkt_queue();
    }
    if ( enc_ )
    {
        // Drain the encoder: send EOF and collect EVERY pending packet
        // until AVERROR_EOF, so the rkmpp wrapper releases its
        // queued MppFrames while its own MPP context is still alive.
        // Without the flush the wrapper tears down its pools first and
        // then runs mpp_frame_deinit on the leftovers — "non-positive
        // ref_count" / "invalid mem pool ptr" at exit.
        auto* ctx = enc_;
        // Drain only if frames were actually encoded: EOF on an idle
        // encoder makes the wrapper's flush path touch an already-torn-down
        // MPP pool ("invalid mem pool ptr" at exit, idle-run case).
        if ( frames_sent_ > 0 )
        {
            AVPacket* pkt = pkt_;
            // The wrapper runs a NON_BLOCK output timeout (no LOW_DELAY
            // flag): EAGAIN during flush means "frames still in flight",
            // not "done". Stopping at the first EAGAIN leaves up to
            // kBufCount frames + EOS queued inside MPP; mpp_destroy then
            // tears its internal queues down over live packets
            // ("mpp_meta: put_meta negative ref_count" /
            // "mpp_mem_pool ... caller mpp_packet_deinit" at exit).
            // Bounded: the encoder holds at most kBufCount frames and
            // codes ~1-2 ms each, so the guards below can never loop
            // forever.
            // EOF must reach the wrapper: on EAGAIN from send (input
            // queue full) pull one output packet out first, then retry
            // the EOF.
            for ( int attempt = 0; attempt < 8; ++attempt )
            {
                if ( avcodec_send_frame( ctx, nullptr ) == 0 )
                    break;
                if ( avcodec_receive_packet( ctx, pkt ) == 0 )
                {
                    av_packet_unref( pkt );
                    continue;
                }
                break;  // non-EAGAIN error: drain only as far as it goes
            }
            // Drain until EOF; EAGAIN here = async frames in flight.
            // Telemetry: pulled / eof / eagains make the drain outcome
            // visible for the shutdown-print investigation.
            int pulled = 0;
            bool eof = false;
            int eagains = 0;
            for ( int guard = 0; guard < 512; ++guard )
            {
                const int r = avcodec_receive_packet( ctx, pkt );
                if ( r == 0 )
                {
                    av_packet_unref( pkt );
                    ++pulled;
                    continue;
                }
                if ( r == AVERROR_EOF )
                {
                    eof = true;
                    break;
                }
                if ( r == AVERROR( EAGAIN ) )
                {
                    ++eagains;
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                    continue;
                }
                log_.warn() << "enc: drain receive error, residual frames possible\n";
                break;
            }
            Log::Line line = log_.info();
            line << "enc: drain: " << pulled << " packet(s), " << ( eof ? "EOF reached" : "NO EOF (EAGAIN x" );
            if ( !eof )
                line << eagains << ")";
            line << "\n";
        }
        avcodec_free_context( &enc_ );
    }
    free_frames();
    if ( pkt_ )
        av_packet_free( &pkt_ );
    // The DRM pool releases itself: pool_ is a member sub-object, its
    // destructor runs right after this body and covers the
    // constructor-throw path too.
    avformat_network_deinit();
}

void NetEncoder::serve()
{
    // Empty host = pull mode: listen on TCP, the viewer connects on its own.
    const bool tcp_pull = cfg_.host.empty();
    const std::string url = tcp_pull ? std::string() 
                                     : ( "udp://" + cfg_.host + ':' + std::to_string( cfg_.port ) + "?pkt_size=1316" );

    TcpPullServer tcp( cfg_.port, 2 * 1024 * 1024, 15 );
    if ( tcp_pull )
    {
        if ( !tcp.listen() )
        {
            log_.error() << "enc: tcp listener bind failed on port " << cfg_.port << '\n';
            error_.store( true );
            return;
        }
        log_.info() << "enc: PULL mode: listening on tcp port " << cfg_.port << 
                     " (viewer: ffplay -fflags nobuffer tcp://<board-ip>:" << cfg_.port << ")\n";
    }

    while ( !quit_.load() )
    {
        error_.store( false );

        // Session body extracted (tidy: readability-function-size).
        // continue;/break; became return;: each failure path now falls
        // through to serve()'s trailing print + UDP backoff, same as before.
        run_session( tcp, tcp_pull, url );
        if ( !quit_.load() )
        {
            log_.info() << ( tcp_pull ? "enc: viewer gone, waiting for the next one" 
                                    : "enc: udp path reset, reopening in 500 ms" ) << "\n";

            if ( !tcp_pull )
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        }
    }
}

// Batch C: the pipeline gates GPU box drawing on this (one call site:
// pipeline.cpp).
bool NetEncoder::viewer_active() const
{
    return active_.load();
}

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int64_t ts_us, int src_idx )
{
    if ( quit_.load() || !active_.load() )
        return;
    // The session this publish belongs to: loaded after active_, so a
    // session seen live is seen with its own epoch (run_session bumps the
    // epoch before it stores active_).
    const uint32_t epoch = session_epoch_.load();
    // Multi-view: frames closer than the pacing step only update their
    // tile in the canvas (core/encode_pacer.hpp).
    const bool do_enc = pacer_.admit( ts_us, queue_depth(), epoch );
    if ( do_enc )
    {
        ts_us_ = ts_us;
        cur_ = ( cur_ + 1 ) % kBufCount;
    }

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. In multi-view mode when two
    // cameras finish within a few ms of each other, !do_enc still updates
    // the source's tile in the persistent composite canvas (mb = nullptr)
    // without emitting a duplicate 2-ms-apart H.264 frame.
    if ( !prep_frame( tensor_fd, band, src_idx, do_enc ? pool_.buf[cur_] : nullptr ) )
    {
        error_.store( true );
        {
            std::lock_guard<std::mutex> nlk( pkt_mx_ );
            pkt_cv_.notify_all();  // serve() closes and reopens the stream path
        }
        return;
    }
    if ( !do_enc )
        return;

    std::lock_guard<std::mutex> lk( mx_ );

    if ( !fc_ || !enc_ )
        return;  // viewer dropped while cropping: frame dropped with it

    if ( !send_frame( st_ ) )
    {
        error_.store( true );
        {
            std::lock_guard<std::mutex> nlk( pkt_mx_ );
            pkt_cv_.notify_all();  // serve() closes and reopens the stream path
        }
    }
}

// NO SIGNAL publish (idle): mirrors publish() -- encode/mux under mx_,
// RGA outside it, error path wakes serve(). The caller paces idle
// encodes at a fixed fps; the pacer stamps them in the same budget as
// live ones: a live frame right after an idle one sees a fresh stamp and
// skips the duplicate encode (multi-view window).
// The frame counter and the timing stats never see idle frames.
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs )
{
    const StreamLayout& lay = cfg_.layout;
    if ( quit_.load() || !active_.load() )
        return;
    const uint32_t epoch = session_epoch_.load();  // see publish()
    bool any = false;
    for ( int s = 0; s < lay.nsrc; ++s )
        any = any || ( !absent || s >= nabs || absent[ s ] );
    if ( !any )
        return;  // every tile went live since the tick was scheduled
    if ( !pacer_.admit_idle( ts_us, queue_depth(), epoch ) )
        return;  // encode queue busy: skip this tick
    ts_us_ = ts_us;
    cur_ = ( cur_ + 1 ) % kBufCount;
    if ( !prep_idle_frame( absent, nabs, pool_.buf[ cur_ ] ) )
    {
        error_.store( true );
        {
            std::lock_guard<std::mutex> nlk( pkt_mx_ );
            pkt_cv_.notify_all();  // serve() closes and reopens the stream path
        }
        return;
    }
    std::lock_guard<std::mutex> lk( mx_ );
    if ( !fc_ || !enc_ )
        return;  // viewer dropped while copying: frame dropped with it
    if ( !send_frame( st_ ) )
    {
        error_.store( true );
        {
            std::lock_guard<std::mutex> nlk( pkt_mx_ );
            pkt_cv_.notify_all();  // serve() closes and reopens the stream path
        }
    }
}

bool NetEncoder::prep_idle_frame( const bool* absent, int nabs, MppBuffer mb )
{
    const StreamLayout& lay = cfg_.layout;
    rga_buffer_t idl = wrapbuffer_fd( mpp_buffer_get_fd( idle_pool_.buf[ 0 ] ),
                                      lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    rga_buffer_t out = wrapbuffer_fd( mpp_buffer_get_fd( mb ),
                                      lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    if ( lay.nsrc <= 1 )
    {
        // Single-view: the idle canvas IS the frame; the band stamp below
        // forces the gray refill on live resume (plate pixels must not
        // survive around the live picture).
        buf_bands_[ cur_ ] = LbGeom { 0, 1, 0, 1 };
        const IM_STATUS cs = imcopy( idl, out );
        if ( static_cast<int>( cs ) <= 0 )
        {
            log_.error() << "enc: RGA idle copy failed: " << imStrError( cs ) << "\n";
            return false;
        }
        return true;
    }
    // Multi-view: stamp the plate tile into every absent cell of the
    // persistent composite canvas; live tiles keep their last picture.
    MppBuffer cv = pool_.buf[ kBufCount ];
    rga_buffer_t cvs = wrapbuffer_fd( mpp_buffer_get_fd( cv ),
                                      lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    for ( int s = 0; s < lay.nsrc; ++s )
    {
        if ( absent && s < nabs && !absent[ s ] )
            continue;
        const LbGeom c = cell_rect( lay, s );
        im_rect tile = { c.left, c.top, c.w, c.h };
        const IM_STATUS rs = improcess( idl, cvs, {}, tile, tile, {}, 0 );
        if ( static_cast<int>( rs ) <= 0 )
        {
            log_.error() << "enc: RGA idle tile copy failed: " << imStrError( rs ) << "\n";
            return false;
        }
        buf_bands_[ s ] = LbGeom { 0, 1, 0, 1 };  // forces the gray refill on live resume
    }
    const IM_STATUS cs = imcopy( cvs, out );
    if ( static_cast<int>( cs ) <= 0 )
    {
        log_.error() << "enc: RGA idle composite copy failed: " << imStrError( cs ) << "\n";
        return false;
    }
    return true;
}

// RGA prep for the current frame: crop the letterbox band of the model
// tensor into the ping-pong NV12 buffer mb (or into the persistent
// composite buffer pool_.buf[kBufCount] + DMA copy in multi-view); the
// crop and the target rectangle come from place_tile() (core/encoder_config.hpp).
// GPU draw -> RGA ordering is guaranteed by sync_cpu() in
// OverlaySink::draw_and_publish, which runs before publish() on the
// same pipeline thread.
bool NetEncoder::prep_frame( int tensor_fd, const LbGeom& band, int src_idx, MppBuffer mb )
{
    const StreamLayout& lay = cfg_.layout;
    const bool multi = lay.nsrc > 1;
    const TilePlacement tp = place_tile( lay, band, src_idx );
    const LbGeom& d = tp.dst;

    MppBuffer target = multi ? pool_.buf[kBufCount] : mb;
    LbGeom& prev = buf_bands_[multi ? static_cast<uint32_t>( tp.cell ) : cur_];
    if ( prev.w > 0 && ( prev.left != d.left || prev.top != d.top || prev.w != d.w || prev.h != d.h ) )
    {
        if ( multi )
        {
            const LbGeom c = cell_rect( lay, tp.cell );
            fill_nv12_gray_rect( target, lay.w, lay.vstride, im_rect { c.left, c.top, c.w, c.h } );
        }
        else
            fill_nv12_gray( target );
    }
    prev = d;

    rga_buffer_t src = wrapbuffer_fd( tensor_fd, cfg_.model_w, cfg_.model_h, RK_FORMAT_RGB_888 );
    im_rect srect = { tp.src.left, tp.src.top, tp.src.w, tp.src.h };
    // Explicit strides: the VU plane lands at w * vstride - exactly where
    // the DRM descriptor (init_frames) reports it.
    rga_buffer_t dst = wrapbuffer_fd( mpp_buffer_get_fd( target ), lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
    im_rect drect = { d.left, d.top, d.w, d.h };
    const IM_STATUS rs = improcess( src, dst, {}, srect, drect, {}, 0 );
    if ( static_cast<int>( rs ) <= 0 )
    {
        log_.error() << "enc: RGA convert failed: " << imStrError( rs ) << "\n";
        return false;
    }
    if ( multi && mb != nullptr )
    {
        rga_buffer_t out = wrapbuffer_fd( mpp_buffer_get_fd( mb ), lay.w, lay.h, RK_FORMAT_YCbCr_420_SP, lay.w, lay.vstride );
        const IM_STATUS cs = imcopy( dst, out );
        if ( static_cast<int>( cs ) <= 0 )
        {
            log_.error() << "enc: RGA composite copy failed: " << imStrError( cs ) << "\n";
            return false;
        }
    }
    return true;
}

bool NetEncoder::init_frames()
{
    const StreamLayout& lay = cfg_.layout;
    for ( int i = 0; i < kBufCount; ++i )
    {
        MppBuffer mb = pool_.buf[i];
        AVDRMFrameDescriptor& desc = desc_[i];
        desc.nb_objects = 1;
        desc.objects[0].fd = mpp_buffer_get_fd( mb );
        desc.objects[0].size = mpp_buffer_get_size( mb );
        desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        desc.nb_layers = 1;
        desc.layers[0].format = DRM_FORMAT_NV12;
        desc.layers[0].nb_planes = 2;
        desc.layers[0].planes[0].object_index = 0;
        desc.layers[0].planes[0].offset = 0;
        desc.layers[0].planes[0].pitch = lay.w;
        desc.layers[0].planes[1].object_index = 0;
        desc.layers[0].planes[1].offset = lay.w * lay.vstride;
        desc.layers[0].planes[1].pitch = lay.w;

        AVFrame* fr = av_frame_alloc();
        if ( !fr )
            return false;
        frames_[i] = fr;
        fr->format = AV_PIX_FMT_DRM_PRIME;
        fr->width = lay.w;
        fr->height = lay.h;
        fr->data[0] = reinterpret_cast<uint8_t*>( &desc );
        fr->buf[0] = av_buffer_create(
            reinterpret_cast<uint8_t*>( &desc ), sizeof( desc ), av_noop_free, nullptr, 0 );
        if ( !fr->buf[0] )
            return false;
    }
    mux_pkt_ = av_packet_alloc();
    dup_pkt_ = av_packet_alloc();
    if ( !mux_pkt_ || !dup_pkt_ )
        return false;
    for ( auto*& qp : pkts_.d )
    {
        qp = av_packet_alloc();
        if ( !qp )
            return false;
    }
    return true;
}

void NetEncoder::free_frames()
{
    for ( auto*& fr : frames_ )
    {
        if ( fr )
            av_frame_free( &fr );
    }
    for ( auto*& qp : pkts_.d )
    {
        if ( qp )
            av_packet_free( &qp );
    }
    if ( mux_pkt_ )
        av_packet_free( &mux_pkt_ );
    if ( dup_pkt_ )
        av_packet_free( &dup_pkt_ );
}

int NetEncoder::queue_depth()
{
    std::lock_guard<std::mutex> qlk( pkt_mx_ );
    return pkts_.n;
}

void NetEncoder::clear_pkt_queue()
{
    while ( !pkts_.empty() )
    {
        av_packet_unref( pkts_.front() );
        pkts_.pop();
    }
}

bool NetEncoder::send_frame( AVStream* st )
{
    AVFrame* fr = frames_[cur_];
    const int64_t fpts = pts_.next( ts_us_, frames_sent_ );
    fr->pts = fpts;

    // 7c forced-IDR: a fresh viewer session forces the next encoded frame
    // to be an IDR, so a late join starts within a frame or two instead
    // of waiting for the GOP boundary. The rkmpp wrapper ignores
    // frame->key_frame, so reach MPP through its private context
    // (RKMPPPriv above).
    if ( force_idr_.exchange( false ) )
    {
        RKMPPPriv* priv = static_cast< RKMPPPriv* >( enc_->priv_data );
        if ( priv && priv->mctx && priv->mapi )
        {
            MPP_RET rc = priv->mapi->control( priv->mctx, MPP_ENC_SET_IDR_FRAME, nullptr );
            if ( rc != MPP_OK )
                log_.warn() << "enc: MPP_ENC_SET_IDR_FRAME failed: " << rc << '\n';
        }
    }

    // EAGAIN from send = the rkmpp input queue is full (3-deep task
    // queue under a VFR clump). The FFmpeg contract: pull one output
    // packet out first, then retry the send (the dtor flush loop uses
    // the same pattern for EOF). Was: fatal -> error_ + a full stream
    // session restart over a transient backpressure.
    AVPacket* pkt = pkt_;
    int ret = 0;
    bool submitted = false;
    for ( int attempt = 0; attempt < 8; ++attempt )
    {
        ret = avcodec_send_frame( enc_, fr );
        if ( ret == 0 )
        {
            submitted = true;
            break;
        }
        if ( ret != AVERROR( EAGAIN ) )
            break;
        if ( avcodec_receive_packet( enc_, pkt ) < 0 )
            break;  // nothing ready to drain: bounded retries exhausted
        enqueue_pkt( pkt, st, fpts );
    }
    if ( ret < 0 && ret != AVERROR( EAGAIN ) )
    {
        log_.error() << "enc: send_frame: " << av_err_str( ret ) << '\n';
        return false;
    }
    if ( !submitted )
    {
        // Queue still full with no output to drain: drop the frame
        // instead of tearing the session. IDR on the next frame keeps
        // the stream decodable; advance() keeps the CFR pts monotonic,
        // frames_sent_ stays put (the frame was never encoded).
        log_.warn() << "enc: send_frame: input queue full, frame dropped, IDR next frame\n";
        force_idr_.store( true );
        pts_.advance();
        return true;
    }

    ret = avcodec_receive_packet( enc_, pkt );
    while ( ret >= 0 )  // drains the burst; EAGAIN/EOF end it
    {
        enqueue_pkt( pkt, st, fpts );
        ret = avcodec_receive_packet( enc_, pkt );
    }
    if ( ret != AVERROR( EAGAIN ) && ret != AVERROR_EOF )
    {
        log_.error() << "enc: receive_packet: " << av_err_str( ret ) << '\n';
        return false;
    }
    pts_.advance();
    ++frames_sent_;
    return true;
}

// One finished encoder packet -> the preallocated ring queue for
// serve() to mux. Shared by the EAGAIN retry and the post-send drain
// in send_frame(): the pipeline thread never touches the network
// (av_packet_move_ref, zero per-frame heap allocs).
void NetEncoder::enqueue_pkt( AVPacket* pkt, AVStream* st, int64_t fpts )
{
    pkt->stream_index = st->index;
    pkt->pts = pkt->dts = pts_.packet_ts( fpts );
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        if ( pkts_.full() )
        {
            // Overflow = serve() stalled (slow viewer / Wi-Fi burst).
            // Dropping one packet tears the reference chain: every later
            // P-frame decodes into corruption until the next IDR. Flush
            // the whole queue, re-arm the IDR gate and force an IDR on
            // the next frame: the gap is one frame, not a full GOP.
            const int ndrop = pkts_.n;
            clear_pkt_queue();
            force_idr_.store( true );
            wait_key_ = true;
            ++pkt_drops_;
            log_.warn() << "enc: packet queue overflow: flushed " << ndrop << " pkts (drop " << pkt_drops_ << "), IDR next frame\n";
        }
        AVPacket*& tail = pkts_.push_slot();  // commits n; !full() guaranteed above
        av_packet_unref( tail );
        av_packet_move_ref( tail, pkt );
        pkt_cv_.notify_one();
    }
}

// Batch C follow-up: the mux side of serve(), split out for tidy
// (readability-function-size). drain_queue() owns the wait/pop loop;
// mux_write() writes one packet with the late-join IDR-prepend logic
// (ParamSetCache; the Annex B scans are annexb:: in core/param_set_cache.hpp).
void NetEncoder::drain_queue( AVFormatContext* fc )
{
    // Session-start gate, shared with the overflow path in send_frame:
    // armed here (late join mid-GOP must not decode P-frames against a
    // missing reference), re-armed by the queue flush, cleared on the
    // first IDR reaching the queue front. Guarded by pkt_mx_ on both
    // sides (serve thread here, pipeline thread in send_frame).
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        wait_key_ = true;
    }
    while ( !quit_.load() && !error_.load() )  // same predicate as the wait below
    {
        size_t q_rem = 0;
        {
            std::unique_lock<std::mutex> plk( pkt_mx_ );
            pkt_cv_.wait( plk, [this] {
                return quit_.load() || error_.load() || pkts_.n > 0;
            } );
            if ( quit_.load() || error_.load() )
                return;
            AVPacket* front = pkts_.front();
            if ( wait_key_ && !annexb::is_idr( front->data, static_cast<size_t>( front->size ) ) )
            {
                av_packet_unref( front );
                pkts_.pop();
                continue;
            }
            wait_key_ = false;
            av_packet_unref( mux_pkt_ );
            av_packet_move_ref( mux_pkt_, front );
            pkts_.pop();
            q_rem = pkts_.n;
        }
        // Ownership of the packet buffer moved into mux_pkt_ via
        // av_packet_move_ref; mux_write consumes and unrefs it without
        // freeing the preallocated AVPacket struct.
        const auto mw_t0 = std::chrono::steady_clock::now();
        const int r = mux_write( fc, mux_pkt_ );
        const long mw_ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - mw_t0 ).count();
        if ( mw_ms > 20 )
            log_.warn() << "enc: mux_write stalled " << mw_ms << " ms (queue was " << q_rem << ")\n";
        if ( r < 0 )
        {
            if ( r == AVERROR( EPIPE ) )
                log_.info() << "enc: viewer disconnected (tcp), session ends\n";
            else if ( r == AVERROR( ETIMEDOUT ) )
                log_.info() << "enc: viewer stalled (send timeout), session ends\n";
            else
                log_.error() << "enc: write failed: " << av_err_str( r ) << " (packet lost)\n";
            error_.store( true );
            return;
        }
    }
}

int NetEncoder::mux_write( AVFormatContext* fc, AVPacket* pkt )
{
    // Late-join support: ParamSetCache captures the SPS/PPS prefix of the
    // first IDR and prepends it to every later IDR that does not carry
    // parameter sets of its own (core/param_set_cache.hpp); the prepended
    // copy goes out through dup_pkt_. The TS muxer tolerates the duplicate.
    AVPacket* out = pkt;
    const ByteSpan b = params_.prepare( pkt->data, static_cast<size_t>( pkt->size ) );
    if ( b.data != pkt->data )
    {
        av_packet_copy_props( dup_pkt_, pkt );
        dup_pkt_->buf = nullptr;
        dup_pkt_->data = b.data;
        dup_pkt_->size = static_cast<int>( b.size );
        out = dup_pkt_;
    }
    const int r = av_write_frame( fc, out );
    dup_pkt_->data = nullptr;
    dup_pkt_->size = 0;
    av_packet_unref( dup_pkt_ );
    av_packet_unref( pkt );
    return r;
}

// One viewer-session iteration: allocate the muxer context, open the
// transport (TCP pull or UDP push), set up the stream and drain the
// encoder packet queue into the muxer. Context and transport teardown
// is RAII (FcGuard + Transport destructor): every early
// return below releases exactly what the former close_transport();
// avformat_free_context() paths released, in the same order.

// Transport teardown: one point, shared by the destructor and the
// session lifetimes. Verbatim from the former close_transport lambda
// in run_session(); members replace the lambda captures.
NetEncoder::Transport::~Transport()
{
    close();
}

void NetEncoder::Transport::close()
{
    if ( tcp_pull )
    {
        if ( tcp )
            tcp->close_client();
    }
    else if ( udp_pb )
    {
        avio_closep( &udp_pb );
    }
}

// Out-of-line dtor of the class-local FcGuard: 
// nulls pb first (the Transport destructor owns the avio context),
// then frees the muxer context.
NetEncoder::FcGuard::~FcGuard()
{
    teardown_muxer( p );  // muxer_teardown.hpp: pb is detached first (the
                          // Transport destructor owns the avio context)
}

// Transport setup, verbatim move out of run_session(): TCP pull
// (poll the listener, accept, socket options, custom AVIO over the
// fd) or UDP push (avio_open, no handshake). The sleep on the UDP
// failure path stays here, exactly where it was in run_session().
bool NetEncoder::open_transport( const std::string& url, AVFormatContext* fc, Transport& t )
{
    if ( t.tcp_pull )
    {
        if ( !t.tcp || !t.tcp->accept_client( quit_ ) )
            return false;

        log_.info() << "enc: viewer connected (tcp)\n";
        force_idr_.store( true );  // next encoded frame must be an IDR

        return t.tcp->attach_avio( fc );
    }

    // UDP: no handshake and no listener — the socket is opened once,
    // packets are pushed regardless of whether a viewer is listening.
    if ( const int ret = avio_open( &t.udp_pb, url.c_str(), AVIO_FLAG_WRITE ); ret < 0 )
    {
        log_.warn() << "enc: udp open failed: " << av_err_str( ret ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return false;
    }
    fc->pb = t.udp_pb;
    return true;
}

// Stream setup, verbatim move out of run_session(): create the
// stream, fill the codec parameters, write the header with the
// live-stream mpegts options, derive the pts step and print the
// session log lines. false = any of the three setup failures.
bool NetEncoder::setup_stream( AVFormatContext* fc, AVStream** st_out, int64_t* frame_step, bool tcp_pull )
{
    const StreamLayout& lay = cfg_.layout;
    AVStream* st = avformat_new_stream( fc, nullptr );
    if ( !st )
        return false;
    // codecpar is allocated by avformat_new_stream itself
    //(libavformat/options.c: st->codecpar = 
    // avcodec_parameters_alloc(), with its own fail path):
    // the former manual fallback here was dead code.
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->format = AV_PIX_FMT_NV12;
    st->codecpar->width = lay.w;
    st->codecpar->height = lay.h;
    st->codecpar->bit_rate = cfg_.bit_rate();  // the codec context belongs to the pipeline thread
    st->time_base = cfg_.vfr() ? AVRational { 1, 90000 } : AVRational { 1, cfg_.fps };
    st->avg_frame_rate = cfg_.vfr() ? AVRational { 0, 1 } : AVRational { cfg_.fps, 1 };

    // Live-stream patch: repeat PAT/PMT/SDT in the stream so a receiver
    // can join at any point, not only at stream startup.
    AVDictionary* mux_opts = nullptr;
    av_dict_set( &mux_opts, "mpegts_flags", "+pat_pmt_at_frames+resend_headers", 0 );
    av_dict_set( &mux_opts, "pat_period", "0.1", 0 );
    av_dict_set( &mux_opts, "sdt_period", "0.25", 0 );
    int ret2 = avformat_write_header( fc, &mux_opts );
    av_dict_free( &mux_opts );
    if ( ret2 < 0 )
    {
        log_.error() << "enc: write_header failed: " << av_err_str( ret2 ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return false;
    }
    const AVRational tb = st->time_base;
    int64_t step = cfg_.vfr() ? 1 : av_rescale_q( 1, AVRational { 1, cfg_.fps }, tb );
    if ( step < 1 )
        step = 1;
    if ( cfg_.vfr() )
        log_.info() << "enc: stream time_base " << tb.num << '/' << tb.den << " (VFR: pts from dequeue times)\n";
    else
        log_.info() << "enc: stream time_base " << tb.num << '/' << tb.den << ", pts step " << step << "\n";
    if ( tcp_pull )
    {
        log_.info() << "enc: tcp pull streaming " << lay.w << 'x' << lay.h << " h264 (mpegts) on port " << cfg_.port << '\n';
    }
    else
    {
        log_.info() << "enc: udp streaming " << lay.w << 'x' << lay.h << " h264 (mpegts) -> " << cfg_.host << ':' << cfg_.port << '\n';
    }
    *st_out = st;
    *frame_step = step;
    return true;
}

// One viewer-session iteration: allocate the muxer context, open the
// transport (TCP pull or UDP push), set up the stream and drain the
// encoder packet queue into the muxer. Context and transport teardown
// is RAII (FcGuard + Transport destructor): every early
// return below releases exactly what the former close_transport();
// avformat_free_context() paths released, in the same order.
void NetEncoder::run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url )
{
    AVFormatContext* raw = nullptr;
    if ( avformat_alloc_output_context2( &raw, nullptr, "mpegts", tcp_pull ? nullptr : url.c_str() ) < 0 || !raw )
    {
        log_.error() << "enc: avformat_alloc_output_context2 failed\n";
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return;
    }
    // Live stream: the muxer must not hold packets for batching.
    // Defaults (max_delay 0.7 s, preload 0.5 s, buffered file I/O)
    // make the TS writer emit data in bursts - on Wi-Fi that reads as
    // periodic client-side buffering. Flush every packet instead.
    raw->max_delay = 0;
    raw->flush_packets = 1;
    
    // Local guard instead of unique_ptr: the Transport pattern used
    // above; same declaration position -> same destruction order
    // (fc dies AFTER the Transport, as before).
    FcGuard fc { raw };

    Transport t;
    t.tcp = &tcp;
    t.tcp_pull = tcp_pull;
    if ( !open_transport( url, fc.p, t ) )
        return;

    AVStream* st = nullptr;
    int64_t frame_step = 1;
    if ( !setup_stream( fc.p, &st, &frame_step, tcp_pull ) )
        return;

    {
        std::lock_guard<std::mutex> lk( mx_ );
        fc_ = fc.p;
        st_ = st;
        pts_.restart( frame_step );  // CFR counter and VFR base restart from zero per session
        session_epoch_.fetch_add( 1 );  // the pacer starts the new session's budget from scratch
        active_.store( true );
    }

    // Batch C: drain the encoder packet queue into the muxer. The
    // pipeline thread never blocks on the network from here on: a slow
    // viewer stalls this loop, not publish(). The wait/pop loop and the
    // per-packet write with the IDR-prepend logic moved to drain_queue()
    // and mux_write() (tidy: readability-function-size).
    drain_queue( fc.p );

    {
        std::lock_guard<std::mutex> lk( mx_ );
        active_.store( false );
        fc_ = nullptr;
        st_ = nullptr;
    }
    // Leftover queued packets are dead weight now: unref them.
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        clear_pkt_queue();
    }
    if ( !error_.load() )
        av_write_trailer( fc.p );  // best effort on a healthy socket
}
