#include <vector>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <im2d.h>
#include <rga.h>
#include <string>
constexpr int kPktQDepth = 8;  // encoder -> serve packet queue depth (frames)

#include <memory>

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

#include "net_encoder.hpp"
#include <cstddef>  // offsetof

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

#include "av_common.hpp"

namespace
{
    void noop_free( void*, uint8_t* ) {}

    // M3.5 pull: avio write callback поверх принятого TCP-сокета.
    int tcp_write_cb( void* opaque, const uint8_t* buf, int buf_size )
    {
        const int fd = *static_cast<int*>( opaque );
        const ssize_t n = ::send( fd, buf, static_cast<size_t>( buf_size ), MSG_NOSIGNAL );
        if ( n > 0 )
            return static_cast<int>( n );
        // Map the send failure: a peer-gone disconnect (EPIPE /
        // ECONNRESET) and a stuck viewer (SO_SNDTIMEO hit ->
        // EAGAIN/EWOULDBLOCK) are normal session events, not I/O
        // faults; drain_queue() reports them as such.
        if ( errno == EPIPE || errno == ECONNRESET )
            return AVERROR( EPIPE );
        if ( errno == EAGAIN || errno == EWOULDBLOCK )
            return AVERROR( ETIMEDOUT );
        return AVERROR( EIO );
    }

    int open_listener( int port )
    {
        const int fd = ::socket( AF_INET, SOCK_STREAM, 0 );
        if ( fd < 0 )
            return -1;
        int one = 1;
        ::setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl( INADDR_ANY );
        addr.sin_port = htons( static_cast<uint16_t>( port ) );
        if ( ::bind( fd, reinterpret_cast<sockaddr*>( &addr ), sizeof( addr ) ) < 0 || ::listen( fd, 1 ) < 0 )
        {
            ::close( fd );
            return -1;
        }
        return fd;
    }

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
    // postprocess scale — audit item 3). The clamps below stay as
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

    // Three NV12 dma-bufs from an MPP DRM pool (P1-4): a plane held by an
    // older encode is never overwritten by RGA on the next frame; the
    // third buffer absorbs VFR bursts where two ran out exactly at the
    // 3-deep rkmpp task queue.
    if ( mpp_buffer_group_get( &buf_group_, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, MODULE_TAG, __FUNCTION__ ) != MPP_OK )
    {
        throw std::runtime_error( "net encoder: mpp_buffer_group_get failed" );
    }
    vstride_ = ( h_ + 15 ) & ~15;  // MPP encoders want a 16-aligned ver_stride
    const size_t bytes = static_cast<size_t>( w_ ) * vstride_ * 3 / 2;
    for ( auto& b : bufs_ )
    {
        if ( mpp_buffer_get( buf_group_, &b, bytes ) != MPP_OK )
        {
            mpp_buffer_group_put( buf_group_ );
            buf_group_ = nullptr;
            throw std::runtime_error( "net encoder: mpp_buffer_get failed" );
        }
        // Neutral gray fill: the stream width is 16-aligned up, the tail
        // columns never receive RGA data — without the fill they would
        // show stale frames at session start.
        if ( void* p = mpp_buffer_get_ptr( b ) )
            std::memset( p, 128, bytes );
    }

    const AVCodec* codec = avcodec_find_encoder_by_name( "h264_rkmpp" );
    if ( !codec )
    {
        throw std::runtime_error( "net encoder: h264_rkmpp not found in this ffmpeg" );
    }
    auto* ctx = avcodec_alloc_context3( codec );
    if ( !ctx )
    {
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
    // M4: net-fps 0 = follow the source. The codec gets the native
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
    // gop_size reaches MPP as rc:gop (rkmpp_set_enc_cfg,
    // line ~452 of libavcodec/rkmppenc.c). Verified empirically:
    // I-frames every 26 in a captured stream (GOP 25).
    ctx->gop_size = vfr_ ? 25 : fps_;  // 1 s: a late joiner waits at most one GOP
    ctx->max_b_frames = 0;  // no reorder: minimal latency
    // Rate control: this ffmpeg-rockchip build has no rc_mode AVOption, so
    // the old generic-option call here never took effect ("option not
    // accepted") and the encoder silently ran with the MPP default (VBR).
    // The real knob is the wrapper's own MppEncCfg, reachable through its
    // private context (RKMPPPriv above, the same channel as the forced-IDR
    // path); CBR is applied after avcodec_open2 below.
    if ( avcodec_open2( ctx, codec, nullptr ) < 0 )
    {
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

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if ( !frame_ || !pkt_ )
    {
        throw std::runtime_error( "net encoder: av_frame/av_packet_alloc failed" );
    }

    thread_ = std::thread( &NetEncoder::serve, this );
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
        // until AVERROR_EOF (P2-a), so the rkmpp wrapper releases its
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
            // not "done". Stopping at the first EAGAIN left up to 3
            // frames + EOS queued inside MPP; mpp_destroy then tore its
            // internal queues down over live packets (the shutdown
            // prints "mpp_meta: put_meta negative ref_count" /
            // "mpp_mem_pool ... caller mpp_packet_deinit",
            // 2026-11-02, phase-dependent). Bounded: the encoder holds
            // at most kBufCount frames and codes ~1-2 ms each, so the
            // guards below can never loop forever.
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
            // P2-b telemetry: pulled / eof / eagains make the drain outcome
            // visible - the shutdown-print investigation needs to know
            // whether EOF was reached at all (the P2-a fix alone did NOT
            // silence the prints, Ctrl-C phase test 2026-11-03).
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
            // P2-b: the drain verdict goes to stdout for the phase-test record.
            std::cout << "enc: drain: " << pulled << " packet(s), "
                      << ( eof ? "EOF reached" : "NO EOF (EAGAIN x" ) ;
            if ( !eof )
                std::cout << eagains << ")";
            std::cout << "\n";
        }
        avcodec_free_context( reinterpret_cast<AVCodecContext**>( &enc_ ) );
    }
    if ( frame_ )
        av_frame_free( reinterpret_cast<AVFrame**>( &frame_ ) );
    if ( pkt_ )
        av_packet_free( reinterpret_cast<AVPacket**>( &pkt_ ) );
    for ( auto& b : bufs_ )
    {
        if ( b )
        {
            mpp_buffer_put( b );
            b = nullptr;
        }
    }
    if ( buf_group_ )
    {
        mpp_buffer_group_put( buf_group_ );
        buf_group_ = nullptr;
    }
    avformat_network_deinit();
}

void NetEncoder::serve()
{
    // Пустой host_ = pull-режим: слушаем TCP, зритель подключается сам.
    const bool tcp_pull = host_.empty();
    const std::string url = tcp_pull ? std::string() 
                                     : ( "udp://" + host_ + ':' + std::to_string( port_ ) + "?pkt_size=1316" );

    int lfd = -1;
    if ( tcp_pull )
    {
        lfd = open_listener( port_ );
        if ( lfd < 0 )
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
        run_session( lfd, tcp_pull, url );
        if ( !quit_.load() )
        {
            std::cout << ( tcp_pull ? "enc: viewer gone, waiting for the next one" 
                                    : "enc: udp path reset, reopening in 500 ms" ) << "\n";

            if ( !tcp_pull )
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        }
    }
    if ( lfd >= 0 )
        ::close( lfd );
}

void NetEncoder::publish( int tensor_fd, int acquire_fence_fd, int64_t ts_us )
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
    // sole user of cur_/bufs_/acq_fd_ here; serve() touches none of
    // them, and the destructor joins this thread before freeing.
    if ( !prep_frame( tensor_fd, bufs_[cur_] ) )
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
bool NetEncoder::prep_frame( int tensor_fd, MppBuffer mb )
{
    rga_buffer_t src = wrapbuffer_fd( tensor_fd, model_w_, model_h_, RK_FORMAT_RGB_888 );
    im_rect srect = { 0, 0, 0, 0 };
    srect.x = band_left_;
    srect.y = band_top_;
    srect.width = band_w_;
    srect.height = band_h_;
    // Explicit strides: the VU plane lands at w * vstride_ - exactly where
    // the DRM descriptor below reports it.
    rga_buffer_t dst = wrapbuffer_fd( mpp_buffer_get_fd( mb ), w_, h_, RK_FORMAT_YCbCr_420_SP, w_, vstride_ );

    if ( acq_fd_ >= 0 )
    {
        pollfd pf {};
        pf.fd = acq_fd_;
        pf.events = POLLIN;
        if ( ::poll( &pf, 1, 200 ) <= 0 )
            std::cerr << "enc: draw fence wait failed, boxes may lag a frame\n";
        ::close( acq_fd_ );
        acq_fd_ = -1;
    }
    // No scaling: the dst rect matches the src band size, so the
    // 16-aligned tail columns of the stream keep their gray fill.
    im_rect drect = { 0, 0, band_w_, band_h_ };
    const IM_STATUS rs = improcess( src, dst, {}, srect, drect, {}, 0 );
    if ( (int)rs <= 0 )
    {
        std::cerr << "enc: RGA convert failed: " << imStrError( rs ) << "\n";
        return false;
    }

    return true;
}

bool NetEncoder::send_frame( AVStream* stp )
{
    auto* st = static_cast<AVStream*>( stp );
    auto* ctx = static_cast<AVCodecContext*>( enc_ );

    MppBuffer mb = bufs_[cur_];  // rotated and RGA-filled by publish/prep_frame

    AVFrame* fr = static_cast<AVFrame*>( frame_ );
    // DRM_PRIME path: hand the wrapper our ping-pong dma-buf by fd. The
    // descriptor is a per-buffer member reused every send - the wrapper
    // clones the frame at submit time and never re-reads the descriptor
    // afterwards (the first frame only, cfg_init gate), so reuse while an
    // older clone is still queued is safe: the clone holds the no-op
    // AVBufferRef for refcounting only. The wrapper imports the fd into
    // its own MPP group and releases it with the packet INPUT_FRAME meta.
    AVDRMFrameDescriptor& desc = desc_[cur_];
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
    fr->format = AV_PIX_FMT_DRM_PRIME;
    fr->width = w_;
    fr->height = h_;
    // M4: CFR — sequential pts in the codec time base; VFR — real dequeue
    // times (µs -> 1/90000 ticks), per-session base, fallback to a 25 fps
    // grid if the pipeline did not provide a timestamp.
    const int64_t fpts = packet_pts();
    fr->pts = fpts;
    fr->data[0] = reinterpret_cast<uint8_t*>( &desc );
    fr->buf[0] = av_buffer_create( reinterpret_cast<uint8_t*>( &desc ), sizeof( desc ), noop_free, nullptr, 0 );
    if ( !fr->buf[0] )
    {
        std::cerr << "enc: av_buffer_create failed\n";
        return false;
    }

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

    int ret = avcodec_send_frame( ctx, fr );
    av_frame_unref( fr );  // drop the buffer ref; the tensor/MPP memory stays
    if ( ret < 0 )
    {
        std::cerr << "enc: send_frame: " << av_err_str( ret ) << '\n';
        return false;
    }

    AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
    ret = avcodec_receive_packet( ctx, pkt );
    while ( ret >= 0 )  // drains the burst; EAGAIN/EOF end it
    {
        pkt->stream_index = st->index;
        pkt->pts = pkt->dts = fpts * frame_step_;
        // Batch C: the pipeline thread no longer touches the network.
        // Clone the packet (deep copy) and hand it to serve()'s drain
        // loop; the original is unreffed immediately. Mux-side header
        // capture/prepend moved to serve() with the rest of the mux.
        AVPacket* copy = av_packet_clone( pkt );
        if ( copy )
        {
            std::lock_guard<std::mutex> plk( pkt_mx_ );
            if ( pkts_.size() >= kPktQDepth )
            {
                // Overflow = serve() stalled (slow viewer / Wi-Fi burst).
                // Dropping one packet tears the reference chain: every later
                // P-frame decodes into corruption until the next IDR. Flush
                // the whole queue, re-arm the IDR gate and force an IDR on
                // the next frame: the gap is one frame, not a full GOP.
                const size_t ndrop = pkts_.size();
                for ( AVPacket* q : pkts_ )
                    av_packet_free( &q );
                pkts_.clear();
                force_idr_.store( true );
                wait_key_ = true;
                ++pkt_drops_;
                // diagnostic: every flush logged
                    std::cerr << "enc: packet queue overflow: flushed " << ndrop << " pkts (drop " << pkt_drops_ << "), IDR next frame\n";
            }
            pkts_.push_back( copy );
            pkt_cv_.notify_one();
        }
        av_packet_unref( pkt );
        ret = avcodec_receive_packet( ctx, pkt );
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
        AVPacket* p = nullptr;
        {
            std::unique_lock<std::mutex> plk( pkt_mx_ );
            pkt_cv_.wait( plk, [this] {
                return quit_.load() || error_.load() || !pkts_.empty();
            } );
            if ( quit_.load() || error_.load() )
                return;
            if ( wait_key_ && !is_idr( pkts_.front() ) )
            {
                av_packet_free( &pkts_.front() );
                pkts_.pop_front();
                continue;
            }
            wait_key_ = false;
            p = pkts_.front();
            pkts_.pop_front();
        }
        // Takes ownership of p: freed inside on every path.
        const auto mw_t0 = std::chrono::steady_clock::now();
        const int r = mux_write( fc, p );
        const long mw_ms = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - mw_t0 ).count();
        if ( mw_ms > 20 )
            std::cerr << "enc: mux_write stalled " << mw_ms << " ms (queue was " << pkts_.size() << ")\n";
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
    AVPacket* dup = nullptr;
    if ( is_idr( pkt ) )
    {
        if ( hdr_.empty() && hdr_capture( pkt, hdr_ ) )
            std::cout << "enc: h264 header captured: " << hdr_.size()
                      << " bytes, repeating on every IDR\n";
        else if ( !hdr_.empty() && !starts_with( pkt, hdr_ ) )
        {
            dup = av_packet_alloc();
            if ( dup && av_new_packet( dup, hdr_.size() + (size_t)pkt->size ) >= 0 )
            {
                memcpy( dup->data, hdr_.data(), hdr_.size() );
                memcpy( dup->data + hdr_.size(), pkt->data, pkt->size );
                av_packet_copy_props( dup, pkt );
                if ( !hdr_logged_ )
                {
                    std::cout << "enc: header repeat active\n";
                    hdr_logged_ = true;
                }
            }
            else
            {
                av_packet_free( &dup );
                dup = nullptr;
            }
        }
    }
    const int r = av_interleaved_write_frame( fc, dup ? dup : pkt );
    av_packet_free( &pkt );
    av_packet_free( &dup );
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
        if ( pb )
            avio_context_free( &pb );
        if ( iobuf )
        {
            av_free( iobuf );
            iobuf = nullptr;
        }
        if ( cfd >= 0 )
        {
            ::close( cfd );
            cfd = -1;
        }
    }
    else if ( pb )
    {
        avio_closep( &pb );
    }
}

// Transport setup, verbatim move out of run_session(): TCP pull
// (poll the listener, accept, socket options, custom AVIO over the
// fd) or UDP push (avio_open, no handshake). The sleep on the UDP
// failure path stays here, exactly where it was in run_session().
bool NetEncoder::open_transport( int lfd, const std::string& url, AVFormatContext* fc, Transport& t )
{
    if ( t.tcp_pull )
    {
        // Ждём зрителя; poll не даёт циклу крутиться вхолостую и не
        // блокирует выход по quit_.
        pollfd pf {};
        pf.fd = lfd;
        pf.events = POLLIN;

        while ( !quit_.load() && ::poll( &pf, 1, 200 ) == 0 )
        {}

        if ( quit_.load() )
            return false;

        t.cfd = ::accept4( lfd, nullptr, nullptr, SOCK_CLOEXEC );
        if ( t.cfd < 0 )
            return false;

        int one = 1;
        ::setsockopt( t.cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
        const int sndbuf = 2 * 1024 * 1024;  // ~10 s of jitter buffering at 1500 kbps
        ::setsockopt( t.cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof( sndbuf ) );
        const timeval tv { 15, 0 };  // dead viewer eventually releases the socket; live jitter is absorbed by sndbuf
        ::setsockopt( t.cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
        std::cout << "enc: viewer connected (tcp)\n";
        force_idr_.store( true );  // next encoded frame must be an IDR

        t.iobuf = static_cast<uint8_t*>( av_malloc( 32768 ) );
        t.pb = t.iobuf ? avio_alloc_context( t.iobuf, 32768, 1, &t.cfd, nullptr, tcp_write_cb, nullptr ) : nullptr;
        if ( !t.pb )
            return false;
        fc->pb = t.pb;
        fc->flags |= AVFMT_FLAG_CUSTOM_IO;

        return true;
    }

    // UDP: без рукопожатия и слушателя — сокет открывается один
    // раз, пакеты пушатся вне зависимости от зрителя.
    const int ret = avio_open( &t.pb, url.c_str(), AVIO_FLAG_WRITE );
    if ( ret < 0 )
    {
        std::cerr << "enc: udp open failed: " << av_err_str( ret ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return false;
    }
    fc->pb = t.pb;
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

    // Live-stream patch: повторять PAT/PMT/SDT в эфире, чтобы приёмник
    // мог подключиться в любой момент, а не только на старте потока.
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
void NetEncoder::run_session( int lfd, bool tcp_pull, const std::string& url )
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
    struct FcGuard
    {
        AVFormatContext* p;
        ~FcGuard()
        {
            if ( p )
                avformat_free_context( p );
        }
        FcGuard( const FcGuard& ) = delete;
        FcGuard& operator=( const FcGuard& ) = delete;
    } fc { raw };

    Transport t;
    t.tcp_pull = tcp_pull;
    if ( !open_transport( lfd, url, fc.p, t ) )
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
    // Leftover queued packets are dead weight now: free them.
    {
        std::lock_guard<std::mutex> plk( pkt_mx_ );
        for ( AVPacket* q : pkts_ )
            av_packet_free( &q );
        pkts_.clear();
    }
    av_write_trailer( fc.p );  // best effort
}
