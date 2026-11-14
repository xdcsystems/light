#include <chrono>
#include <cstddef>  // offsetof
#include <cstdint>
#include <stdexcept>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>  // avformat_network_init / deinit
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/log.hpp"
#include "core/pts_clock.hpp"
#include "net/encoder_frame_pool.hpp"
#include "net/frame_composer.hpp"
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


NetEncoder::NetEncoder( const EncoderConfig& cfg, const Log& log )
  : log_( log )
  , cfg_( cfg )
  , ring_( cfg.layout )
  , composer_( cfg, log )
  , pacer_( cfg.layout.nsrc, cfg.fps )
  , pts_( cfg.vfr() )
  , session_( cfg, queue_, gate_, log )
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

    // The input ring (ring_) and the canvases (composer_) are allocated by
    // their own ctors, before this body: RAII members, a throw below
    // (avcodec_open2, av_packet_alloc) cannot leak them.

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
    if ( !pkt_ )
    {
        avcodec_free_context( &enc_ );
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: av_packet_alloc failed" );
    }

    try
    {
        session_.start();  // the serve thread: last, once the encoder is ready
    }
    catch ( ... )
    {
        av_packet_free( &pkt_ );
        avcodec_free_context( &enc_ );
        avformat_network_deinit();
        throw;
    }
}

NetEncoder::~NetEncoder()
{
    session_.stop();  // the serve thread ends first: quit, queue stop, join
    // Queued packets reference encoder output buffers: drop them while the
    // codec is alive (the drain below frees it).
    queue_.close();
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
            // EncoderFramePool::kSlots frames + EOS queued inside MPP; mpp_destroy then
            // tears its internal queues down over live packets
            // ("mpp_meta: put_meta negative ref_count" /
            // "mpp_mem_pool ... caller mpp_packet_deinit" at exit).
            // Bounded: the encoder holds at most kSlots frames and
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
    if ( pkt_ )
        av_packet_free( &pkt_ );
    // The dma-bufs release themselves: composer_ (the canvases) and ring_
    // (the input ring + its AVFrames) are members, destroyed right after
    // this body -- after the codec that references them -- and on the
    // constructor-throw path too.
    avformat_network_deinit();
}


// Batch C: the pipeline gates GPU box drawing on this (one call site:
// pipeline.cpp).
bool NetEncoder::viewer_active() const
{
    return gate_.active();
}

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int64_t ts_us, int src_idx )
{
    if ( !gate_.active() )
        return;
    // The session this publish belongs to: loaded after active(), so a
    // session seen live is seen with its own epoch (SessionGate::open
    // stores the epoch first). The pacer and a fail() use it.
    const uint32_t epoch = gate_.epoch();
    // Multi-view: frames closer than the pacing step only update their
    // tile in the canvas (core/encode_pacer.hpp).
    const bool do_enc = pacer_.admit( ts_us, queue_.depth(), epoch );
    if ( do_enc )
    {
        ts_us_ = ts_us;
        ring_.advance();
    }

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. In multi-view mode when two
    // cameras finish within a few ms of each other, !do_enc still updates
    // the source's tile in the persistent composite canvas (out = nullptr)
    // without emitting a duplicate 2-ms-apart H.264 frame.
    const int slot = ring_.current();
    if ( !composer_.compose( tensor_fd, band, src_idx, slot, do_enc ? ring_.buf( slot ) : nullptr ) )
    {
        queue_.fail( epoch );  // ends THIS session only: serve() reopens the stream path
        return;
    }
    if ( !do_enc )
        return;

    // No session any more (viewer dropped while cropping): the frame is
    // dropped with it. A send failure ends the session it was sent into.
    gate_.run( [this]( const SessionGate::Session& s ) {
        if ( !send_frame( s ) )
            queue_.fail( s.epoch );
    } );
}

// NO SIGNAL publish (idle): mirrors publish() -- encode under the gate,
// RGA outside it, error path wakes serve(). The caller paces idle
// encodes at a fixed fps; the pacer stamps them in the same budget as
// live ones: a live frame right after an idle one sees a fresh stamp and
// skips the duplicate encode (multi-view window).
// The frame counter and the timing stats never see idle frames.
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs )
{
    const StreamLayout& lay = cfg_.layout;
    if ( !gate_.active() )
        return;
    const uint32_t epoch = gate_.epoch();  // see publish()
    bool any = false;
    for ( int s = 0; s < lay.nsrc; ++s )
        any = any || ( !absent || s >= nabs || absent[ s ] );
    if ( !any )
        return;  // every tile went live since the tick was scheduled
    if ( !pacer_.admit_idle( ts_us, queue_.depth(), epoch ) )
        return;  // encode queue busy: skip this tick
    ts_us_ = ts_us;
    const int slot = ring_.advance();
    if ( !composer_.compose_idle( absent, nabs, slot, ring_.buf( slot ) ) )
    {
        queue_.fail( epoch );  // see publish()
        return;
    }
    gate_.run( [this]( const SessionGate::Session& s ) {  // see publish()
        if ( !send_frame( s ) )
            queue_.fail( s.epoch );
    } );
}

bool NetEncoder::send_frame( const SessionGate::Session& s )
{
    // A new session (its first encode): the pts restart from zero with
    // the session's step, and a TCP viewer's first frame is an IDR.
    if ( s.epoch != seen_epoch_ )
    {
        seen_epoch_ = s.epoch;
        pts_.restart( s.frame_step );
        force_idr_ = force_idr_ || s.force_idr;
    }
    AVFrame* fr = ring_.frame( ring_.current() );
    const int64_t fpts = pts_.next( ts_us_, frames_sent_ );
    fr->pts = fpts;

    // 7c forced-IDR: a fresh viewer session forces the next encoded frame
    // to be an IDR, so a late join starts within a frame or two instead
    // of waiting for the GOP boundary. The rkmpp wrapper ignores
    // frame->key_frame, so reach MPP through its private context
    // (RKMPPPriv above).
    if ( force_idr_ )
    {
        force_idr_ = false;
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
    // the same pattern for EOF). Was: fatal -> a full stream
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
        enqueue_pkt( pkt, s.stream_index, fpts );
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
        force_idr_ = true;
        pts_.advance();
        return true;
    }

    ret = avcodec_receive_packet( enc_, pkt );
    while ( ret >= 0 )  // drains the burst; EAGAIN/EOF end it
    {
        enqueue_pkt( pkt, s.stream_index, fpts );
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
void NetEncoder::enqueue_pkt( AVPacket* pkt, int stream_index, int64_t fpts )
{
    pkt->stream_index = stream_index;
    pkt->pts = pkt->dts = pts_.packet_ts( fpts );
    // Overflow = serve() stalled (slow viewer / Wi-Fi burst): the queue
    // flushed itself and re-armed its key gate (core/packet_queue.hpp);
    // an IDR on the next frame makes the gap one frame, not a full GOP.
    const PacketQueue::Pushed pu = queue_.push( pkt );
    if ( pu.flushed > 0 )
    {
        force_idr_ = true;
        log_.warn() << "enc: packet queue overflow: flushed " << pu.flushed << " pkts (drop " << pu.drops
                    << "), IDR next frame\n";
    }
}

