#include <vector>
#include <chrono>
#include <cstring>
#include <cstddef>  // offsetof
#include <iostream>
#include <sstream>
#include <string>
#include <poll.h>
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
    bool is_idr( const AVPacket* q )
    {
        for ( size_t i = 0; i + 4 <= (size_t)q->size; ++i )
            if ( q->data[i] == 0 && q->data[i+1] == 0 && q->data[i+2] == 1 )
            {
                const uint8_t t = q->data[i+3] & 0x1F;
                if ( t == 5 ) return true;
                if ( t == 6 || t == 7 || t == 8 || t == 9 ) continue;
                return false;
            }
        return false;
    }

    bool hdr_capture( const AVPacket* q, std::vector< uint8_t >& out )
    {
        // Anchor at the first start code: both 3-byte (00 00 01) and
        // 4-byte (00 00 00 01) prefixes occur in the wild; anchoring
        // on 3-byte only silently matched nothing on 4-byte input.
        size_t i = 0;
        while ( i + 3 <= (size_t)q->size &&
                !( q->data[i] == 0 && q->data[i+1] == 0 && q->data[i+2] == 1 ) )
            ++i;
        if ( i + 4 > (size_t)q->size )
            return false;
        bool saw_sps = false;
        while ( i + 4 <= (size_t)q->size && q->data[i] == 0 && q->data[i+1] == 0 && q->data[i+2] == 1 )
        {
            const uint8_t t = q->data[i+3] & 0x1F;
            if ( t == 7 ) saw_sps = true;
            if ( t == 5 || t == 1 || t == 10 ) break;  // slice/EoS NAL: header ends here
            size_t j = i + 3;
            while ( j + 3 <= (size_t)q->size && !( q->data[j] == 0 && q->data[j+1] == 0 && q->data[j+2] == 1 ) ) ++j;
            i = j;
        }
        if ( saw_sps && i > 0 )
        {
            out.assign( q->data, q->data + i );
            return true;
        }
        return false;
    }

    bool starts_with( const AVPacket* q, const std::vector< uint8_t >& h )
    {
        return !h.empty() && q->size >= (int)h.size() && memcmp( q->data, h.data(), h.size() ) == 0;
    }
}  // namespace

NetEncoder::NetEncoder( int model_w, int model_h, const LbGeom& band, int fps, int port, const std::string& host, int bitrate_kbps )
  : w_( ( band.w + 15 ) & ~15 )  // MPP hor_stride: stream width 16-aligned up
  , h_( band.h )
  , fps_( fps )
  , port_( port )
  , bitrate_kbps_( bitrate_kbps > 0 ? bitrate_kbps : 12000 )
  , host_( host )
  , vfr_( fps <= 0 )  // --net-fps 0: follow the source
  , model_w_( model_w )
  , model_h_( model_h )
{
    avformat_network_init();

    // Visible letterbox band inside the tensor: everything outside is the
    // gray padding memset once at startup. The stream is the band at its
    // NATIVE size — no upscale. Band geometry arrives from the Pipeline
    // (single-owner LbGeom, the same numbers as the RGA crop and the
    // postprocess scale). The clamps below stay as
    // defensive guards only.

    band_h_ = band.h;
    band_top_ = band.top;
    band_w_ = band.w;
    band_left_ = band.left;
    if ( band_h_ > model_h_ )
        band_h_ = model_h_;
    if ( band_top_ < 0 )
        band_top_ = 0;
    if ( band_w_ > model_w_ )
        band_w_ = model_w_;
    if ( band_left_ < 0 )
        band_left_ = 0;
    std::cout << "enc: tensor band x=[" << band_left_ << ',' << band_left_ + band_w_ << ") y=[" << band_top_ << ',' << band_top_ + band_h_
              << ") of " << model_w_ << 'x' << model_h_ << " -> " << w_ << 'x' << h_ << '\n';

    // Three NV12 dma-bufs: a plane held by an older encode is never
    // overwritten by RGA on the next frame; the third buffer absorbs
    // VFR bursts where two ran out exactly at the 3-deep rkmpp task
    // queue.
    // RAII sub-object: a throw below (avcodec_open2, av_frame_alloc)
    // cannot leak the pool -- the sub-object destructs on unwinding.
    // Alloc failures throw from here (fail loud, RknnEngine pattern);
    // the pool has already self-cleaned by then.
    vstride_ = ( h_ + 15 ) & ~15;  // MPP encoders want a 16-aligned ver_stride
    std::string pool_why;
    if ( !pool_.alloc( static_cast<size_t>( w_ ) * vstride_ * 3 / 2, MODULE_TAG, __FUNCTION__, &pool_why ) )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net encoder: " + pool_why );
    }

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
    ctx->width = w_;
    ctx->height = h_;
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
    ctx->time_base = vfr_ ? AVRational { 1, 90000 } : AVRational { 1, fps_ };
    // VFR: rc still needs a numeric fps — 0/1 reaches MPP as the auto
    // marker (65535) and the CBR per-frame budget collapses (stream
    // dries to ~83 kbps). Nominal 25 for rate control only; pts stay
    // VFR from dequeue times.
    ctx->framerate = vfr_ ? AVRational { 25, 1 } : AVRational { fps_, 1 };
    ctx->bit_rate = static_cast<int64_t>( bitrate_kbps_ ) * 1000;
    // gop_size reaches MPP as rc:gop (rkmpp_set_enc_cfg):
    // a captured stream shows I-frames every 26 for GOP 25.
    ctx->gop_size = vfr_ ? 25 : fps_;  // 1 s: a late joiner waits at most one GOP
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
    RKMPPPriv* rc_priv = reinterpret_cast< RKMPPPriv* >( ctx->priv_data );
    if ( rc_priv && rc_priv->mctx && rc_priv->mapi && rc_priv->mcfg )
    {
        mpp_enc_cfg_set_s32( rc_priv->mcfg, "rc:mode", MPP_ENC_RC_MODE_CBR );
        const MPP_RET rc_set = rc_priv->mapi->control( rc_priv->mctx, MPP_ENC_SET_CFG, rc_priv->mcfg );
        if ( rc_set == MPP_OK )
            std::cout << "enc: rc mode CBR applied via MPP_ENC_SET_CFG\n";
        else
            std::cout << "enc: rc mode CBR rejected by MPP (rc " << rc_set << "), encoder default stays\n";
    }
    else
    {
        std::cout << "enc: rkmpp priv unavailable, encoder default rc stays\n";
    }
    std::cout << "enc: h264_rkmpp open: " << w_ << 'x' << h_ << ", " << bitrate_kbps_ << " kbps, gop " << ctx->gop_size << '\n';
    if ( vfr_ )
        std::cout << "enc: VFR follow-source mode: time_base 1/90000, pts from dequeue times\n";

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
    // Defensive: close a fence fd if a future edit to publish() adds an
    // early return between the fd hand-off and prep_frame(). Today the
    // fd is always consumed, so this is a no-op.
    if ( acq_fd_ >= 0 )
    {
        ::close( acq_fd_ );
        acq_fd_ = -1;
    }
    if ( enc_ )
    {
        // Drain the encoder: send EOF and collect EVERY pending packet
        // until AVERROR_EOF, so the rkmpp wrapper releases its
        // queued MppFrames while its own MPP context is still alive.
        // Without the flush the wrapper tears down its pools first and
        // then runs mpp_frame_deinit on the leftovers — "non-positive
        // ref_count" / "invalid mem pool ptr" at exit.
        auto* ctx = static_cast<AVCodecContext*>( enc_ );
        // Drain only if frames were actually encoded: EOF on an idle
        // encoder makes the wrapper's flush path touch an already-torn-down
        // MPP pool ("invalid mem pool ptr" at exit, idle-run case).
        if ( frames_sent_ > 0 )
        {
            AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
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
                std::cerr << "enc: drain receive error, residual frames possible\n";
                break;
            }
            std::cout << "enc: drain: " << pulled << " packet(s), "
                      << ( eof ? "EOF reached" : "NO EOF (EAGAIN x" ) ;
            if ( !eof )
                std::cout << eagains << ")";
            std::cout << "\n";
        }
        avcodec_free_context( reinterpret_cast<AVCodecContext**>( &enc_ ) );
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
    // Empty host_ = pull mode: listen on TCP, the viewer connects on its own.
    const bool tcp_pull = host_.empty();
    const std::string url = tcp_pull ? std::string() 
                                     : ( "udp://" + host_ + ':' + std::to_string( port_ ) + "?pkt_size=1316" );

    TcpPullServer tcp( port_, 2 * 1024 * 1024, 15 );
    if ( tcp_pull )
    {
        if ( !tcp.listen() )
        {
            std::cerr << "enc: tcp listener bind failed on port " << port_ << '\n';
            error_.store( true );
            return;
        }
        std::cout << "enc: PULL mode: listening on tcp port " << port_ << 
                     " (viewer: ffplay -fflags nobuffer tcp://<board-ip>:" << port_ << ")\n";
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
            std::cout << ( tcp_pull ? "enc: viewer gone, waiting for the next one" 
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

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int acquire_fence_fd, int64_t ts_us )
{
    if ( quit_.load() || !active_.load() )
    {
        if ( acquire_fence_fd >= 0 )
            ::close( acquire_fence_fd );
        return;
    }

    if ( acq_fd_ >= 0 )
    {
        ::close( acq_fd_ );
        acq_fd_ = -1;
    }
    acq_fd_ = acquire_fence_fd;
    ts_us_ = ts_us;
    cur_ = ( cur_ + 1 ) % kBufCount;

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. The pipeline thread is the
    // sole user of cur_/pool_.buf/acq_fd_ here; serve() touches none of
    // them, and the destructor joins this thread before freeing.
    if ( !prep_frame( tensor_fd, band, pool_.buf[cur_] ) )
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

// RGA prep for the current frame: wait out the GPU draw fence (this RGA
// driver ignores input fences, so ordering is a poll) and crop the
// letterbox band of the model tensor into the ping-pong NV12 buffer mb.
bool NetEncoder::prep_frame( int tensor_fd, const LbGeom& band, MppBuffer mb )
{
    const LbGeom src_b = ( band.w > 0 && band.h > 0 ) ? band : LbGeom { band_left_, band_w_, band_top_, band_h_ };
    LbGeom dst_b = ( src_b.w == band_w_ && src_b.h == band_h_ )
                       ? LbGeom { 0, band_w_, 0, band_h_ }
                       : band_geom( src_b.w, src_b.h, band_w_, band_h_ );
    dst_b.left &= ~1;
    dst_b.top &= ~1;
    dst_b.w &= ~1;
    dst_b.h &= ~1;

    LbGeom& prev = buf_bands_[cur_];
    if ( prev.w > 0 && ( prev.left != dst_b.left || prev.top != dst_b.top || prev.w != dst_b.w || prev.h != dst_b.h ) )
    {
        if ( void* p = mpp_buffer_get_ptr( mb ) )
        {
            const size_t sz = mpp_buffer_get_size( mb );
            dma::DmaSync wr( mpp_buffer_get_fd( mb ), true );
            std::memset( p, 128, sz );
            dma::cache_clean( p, sz );
        }
    }
    prev = dst_b;

    rga_buffer_t src = wrapbuffer_fd( tensor_fd, model_w_, model_h_, RK_FORMAT_RGB_888 );
    im_rect srect = { src_b.left & ~1, src_b.top & ~1, src_b.w & ~1, src_b.h & ~1 };
    // Explicit strides: the VU plane lands at w * vstride_ - exactly where
    // the DRM descriptor below reports it.
    rga_buffer_t dst = wrapbuffer_fd( mpp_buffer_get_fd( mb ), w_, h_, RK_FORMAT_YCbCr_420_SP, w_, vstride_ );

    if ( acq_fd_ >= 0 )
    {
        pollfd pf {};
        pf.fd = acq_fd_;
        pf.events = POLLIN;
        // The pipeline thread is also the camera thread. A long stall
        // here stops DQBUF and the driver drops frames. 8 ms is enough
        // for a healthy Mali fence; on timeout the boxes may lag one
        // frame, which is cheaper than a 200 ms capture freeze.
        if ( ::poll( &pf, 1, 8 ) <= 0 )
            std::cerr << "enc: draw fence wait failed, boxes may lag a frame\n";
        ::close( acq_fd_ );
        acq_fd_ = -1;
    }
    im_rect drect = { dst_b.left, dst_b.top, dst_b.w, dst_b.h };
    const IM_STATUS rs = improcess( src, dst, {}, srect, drect, {}, 0 );
    if ( (int)rs <= 0 )
    {
        std::cerr << "enc: RGA convert failed: " << imStrError( rs ) << "\n";
        return false;
    }

    return true;
}

bool NetEncoder::init_frames()
{
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
        desc.layers[0].planes[0].pitch = w_;
        desc.layers[0].planes[1].object_index = 0;
        desc.layers[0].planes[1].offset = w_ * vstride_;
        desc.layers[0].planes[1].pitch = w_;

        AVFrame* fr = av_frame_alloc();
        if ( !fr )
            return false;
        frames_[i] = fr;
        fr->format = AV_PIX_FMT_DRM_PRIME;
        fr->width = w_;
        fr->height = h_;
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
    for ( auto*& qp : pkts_ )
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
    for ( auto*& qp : pkts_ )
    {
        if ( qp )
            av_packet_free( &qp );
    }
    if ( mux_pkt_ )
        av_packet_free( &mux_pkt_ );
    if ( dup_pkt_ )
        av_packet_free( &dup_pkt_ );
}

void NetEncoder::clear_pkt_queue()
{
    for ( size_t i = 0; i < pkt_count_; ++i )
    {
        av_packet_unref( pkts_[( pkt_head_ + i ) % kPktQDepth] );
    }
    pkt_head_ = 0;
    pkt_count_ = 0;
}

bool NetEncoder::send_frame( AVStream* st )
{
    AVFrame* fr = frames_[cur_];
    const int64_t fpts = packet_pts();
    fr->pts = fpts;

    // 7c forced-IDR: a fresh viewer session forces the next encoded frame
    // to be an IDR, so a late join starts within a frame or two instead
    // of waiting for the GOP boundary. The rkmpp wrapper ignores
    // frame->key_frame, so reach MPP through its private context
    // (RKMPPPriv above).
    if ( force_idr_.exchange( false ) )
    {
        RKMPPPriv* priv = reinterpret_cast< RKMPPPriv* >( enc_->priv_data );
        if ( priv && priv->mctx && priv->mapi )
        {
            MPP_RET rc = priv->mapi->control( priv->mctx, MPP_ENC_SET_IDR_FRAME, nullptr );
            if ( rc != MPP_OK )
                std::cerr << "enc: MPP_ENC_SET_IDR_FRAME failed: " << rc << char(10);
        }
    }

    int ret = avcodec_send_frame( enc_, fr );
    if ( ret < 0 )
    {
        std::cerr << "enc: send_frame: " << av_err_str( ret ) << '\n';
        return false;
    }

    AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
    ret = avcodec_receive_packet( enc_, pkt );
    while ( ret >= 0 )  // drains the burst; EAGAIN/EOF end it
    {
        pkt->stream_index = st->index;
        pkt->pts = pkt->dts = fpts * frame_step_;
        // Batch C: the pipeline thread no longer touches the network.
        // Move the packet reference into the preallocated ring queue
        // (av_packet_move_ref, zero per-frame heap allocs) and hand it
        // to serve()'s drain loop.
        {
            std::lock_guard<std::mutex> plk( pkt_mx_ );
            if ( pkt_count_ >= kPktQDepth )
            {
                // Overflow = serve() stalled (slow viewer / Wi-Fi burst).
                // Dropping one packet tears the reference chain: every later
                // P-frame decodes into corruption until the next IDR. Flush
                // the whole queue, re-arm the IDR gate and force an IDR on
                // the next frame: the gap is one frame, not a full GOP.
                const size_t ndrop = pkt_count_;
                clear_pkt_queue();
                force_idr_.store( true );
                wait_key_ = true;
                ++pkt_drops_;
                std::cerr << "enc: packet queue overflow: flushed " << ndrop << " pkts (drop " << pkt_drops_ << "), IDR next frame\n";
            }
            const size_t tail = ( pkt_head_ + pkt_count_ ) % kPktQDepth;
            av_packet_unref( pkts_[tail] );
            av_packet_move_ref( pkts_[tail], pkt );
            ++pkt_count_;
            pkt_cv_.notify_one();
        }
        ret = avcodec_receive_packet( enc_, pkt );
    }
    if ( ret != AVERROR( EAGAIN ) && ret != AVERROR_EOF )
    {
        std::cerr << "enc: receive_packet: " << av_err_str( ret ) << '\n';
        return false;
    }
    ++pts_;
    ++frames_sent_;
    return true;
}

// M4 pts selection, extracted from send_frame() for readability:
// CFR gets the sequential codec pts; VFR rescales the dequeue
// timestamp (us -> 1/90000 ticks) against the per-session base
// (initialized on the first timestamped packet) or falls back to
// a 25 fps grid.
int64_t NetEncoder::packet_pts()
{
    if ( !vfr_ )
        return static_cast<int64_t>( pts_ );
    if ( ts_us_ < 0 )
        return static_cast<int64_t>( frames_sent_ ) * 3600;  // 25 fps grid
    if ( ts_base_ < 0 )
        ts_base_ = ts_us_;  // per-session base on the first timestamp
    return av_rescale( ts_us_ - ts_base_, 90000, 1000000 );
}

// Batch C follow-up: the mux side of serve(), split out for tidy
// (readability-function-size). drain_queue() owns the wait/pop loop;
// mux_write() writes one packet with the late-join IDR-prepend logic.
// hdr_/hdr_logged_ stay members; is_idr/hdr_capture/starts_with are the
// file-local helpers above.
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
                return quit_.load() || error_.load() || pkt_count_ > 0;
            } );
            if ( quit_.load() || error_.load() )
                return;
            AVPacket* front = pkts_[pkt_head_];
            if ( wait_key_ && !is_idr( front ) )
            {
                av_packet_unref( front );
                pkt_head_ = ( pkt_head_ + 1 ) % kPktQDepth;
                --pkt_count_;
                continue;
            }
            wait_key_ = false;
            av_packet_unref( mux_pkt_ );
            av_packet_move_ref( mux_pkt_, front );
            pkt_head_ = ( pkt_head_ + 1 ) % kPktQDepth;
            --pkt_count_;
            q_rem = pkt_count_;
        }
        // Ownership of the packet buffer moved into mux_pkt_ via
        // av_packet_move_ref; mux_write consumes and unrefs it without
        // freeing the preallocated AVPacket struct.
        const auto mw_t0 = std::chrono::steady_clock::now();
        const int r = mux_write( fc, mux_pkt_ );
        const long mw_ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - mw_t0 ).count();
        if ( mw_ms > 20 )
            std::cerr << "enc: mux_write stalled " << mw_ms << " ms (queue was " << q_rem << ")\n";
        if ( r < 0 )
        {
            if ( r == AVERROR( EPIPE ) )
                std::cout << "enc: viewer disconnected (tcp), session ends\n";
            else if ( r == AVERROR( ETIMEDOUT ) )
                std::cout << "enc: viewer stalled (send timeout), session ends\n";
            else
                std::cerr << "enc: write failed: " << av_err_str( r ) << " (packet lost)\n";
            error_.store( true );
            return;
        }
    }
}

int NetEncoder::mux_write( AVFormatContext* fc, AVPacket* pkt )
{
    // Late-join support (moved from send_frame): capture the
    // parameter-set prefix from the first key packet and prepend it to
    // every following IDR; the TS muxer tolerates the duplicate.
    AVPacket* out = pkt;
    if ( is_idr( pkt ) )
    {
        if ( hdr_.empty() && hdr_capture( pkt, hdr_ ) )
            std::cout << "enc: h264 header captured: " << hdr_.size()
                      << " bytes, repeating on every IDR\n";
        else if ( !hdr_.empty() && !starts_with( pkt, hdr_ ) )
        {
            if ( av_new_packet( dup_pkt_, static_cast<int>( hdr_.size() ) + pkt->size ) >= 0 )
            {
                memcpy( dup_pkt_->data, hdr_.data(), hdr_.size() );
                memcpy( dup_pkt_->data + hdr_.size(), pkt->data, static_cast<size_t>( pkt->size ) );
                av_packet_copy_props( dup_pkt_, pkt );
                out = dup_pkt_;
                if ( !hdr_logged_ )
                {
                    std::cout << "enc: header repeat active\n";
                    hdr_logged_ = true;
                }
            }
            else
            {
                av_packet_unref( dup_pkt_ );
            }
        }
    }
    const int r = av_interleaved_write_frame( fc, out );
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
    if ( p )
    {
        p->pb = nullptr;
        avformat_free_context( p );
    }
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

        std::cout << "enc: viewer connected (tcp)\n";
        force_idr_.store( true );  // next encoded frame must be an IDR

        return t.tcp->attach_avio( fc );
    }

    // UDP: no handshake and no listener — the socket is opened once,
    // packets are pushed regardless of whether a viewer is listening.
    const int ret = avio_open( &t.udp_pb, url.c_str(), AVIO_FLAG_WRITE );
    if ( ret < 0 )
    {
        std::cerr << "enc: udp open failed: " << av_err_str( ret ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return false;
    }
    fc->pb = t.udp_pb;
    return true;
}

// Stream setup, verbatim move out of run_session(): create the
// stream, fill the codec parameters, write the header with the
// live-stream mpegts options, derive frame_step_ and print the
// session log lines. false = any of the three setup failures.
bool NetEncoder::setup_stream( AVFormatContext* fc, AVStream** st_out, bool tcp_pull )
{
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
    st->codecpar->width = w_;
    st->codecpar->height = h_;
    st->codecpar->bit_rate = static_cast<AVCodecContext*>( enc_ )->bit_rate;
    st->time_base = vfr_ ? AVRational { 1, 90000 } : AVRational { 1, fps_ };
    st->avg_frame_rate = vfr_ ? AVRational { 0, 1 } : AVRational { fps_, 1 };

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
        std::cerr << "enc: write_header failed: " << av_err_str( ret2 ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return false;
    }
    const AVRational tb = st->time_base;
    frame_step_ = vfr_ ? 1 : av_rescale_q( 1, AVRational { 1, fps_ }, tb );
    if ( frame_step_ < 1 )
        frame_step_ = 1;
    if ( vfr_ )
        std::cout << "enc: stream time_base " << tb.num << '/' << tb.den << " (VFR: pts from dequeue times)\n";
    else
        std::cout << "enc: stream time_base " << tb.num << '/' << tb.den << ", pts step " << frame_step_ << "\n";
    if ( tcp_pull )
    {
        std::cout << "enc: tcp pull streaming " << w_ << 'x' << h_ << " h264 (mpegts) on port " << port_ << '\n';
    }
    else
    {
        std::cout << "enc: udp streaming " << w_ << 'x' << h_ << " h264 (mpegts) -> " << host_ << ':' << port_ << '\n';
    }
    *st_out = st;
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
        std::cerr << "enc: avformat_alloc_output_context2 failed\n";
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
    if ( !setup_stream( fc.p, &st, tcp_pull ) )
        return;

    {
        std::lock_guard<std::mutex> lk( mx_ );
        fc_ = fc.p;
        st_ = st;
        pts_ = 0;
        ts_base_ = -1;  // M4: VFR pts restart from zero per session
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
    av_write_trailer( fc.p );  // best effort
}
