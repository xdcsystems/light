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
#include <cmath>

#include <im2d.h>
#include <rga.h>

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

namespace
{
    void noop_free( void*, uint8_t* ) {}

    // M3.5 pull: avio write callback поверх принятого TCP-сокета.
    int tcp_write_cb( void* opaque, const uint8_t* buf, int buf_size )
    {
        const int fd = *static_cast<int*>( opaque );
        const ssize_t n = ::send( fd, buf, static_cast<size_t>( buf_size ), MSG_NOSIGNAL );
        return n <= 0 ? AVERROR( EIO ) : static_cast<int>( n );
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

}  // namespace

NetEncoder::NetEncoder( int w, int h, int fps, int port, const std::string& host, int bitrate_kbps, int model_w, int model_h )
  : w_( w )
  , h_( h )
  , fps_( fps > 0 ? fps : 25 )
  , port_( port )
  , bitrate_kbps_( bitrate_kbps > 0 ? bitrate_kbps : 12000 )
  , host_( host )
  , model_w_( model_w )
  , model_h_( model_h )
{
    avformat_network_init();

    // Visible letterbox band inside the tensor: everything outside is the
    // gray padding memset once at startup. The stream is the band at its
    // NATIVE size — no upscale. The pipeline passes w = model_w and
    // h = cam_h * (model_w / cam_w); with that w the scale below is
    // s = model_w / w = 1, so band_h_ == h_ and band_top_ = (model_h_ - h_)/2
    // (640x640 tensor, 1280x720 camera -> 640x360, band y=[140,500)).

    const float s = static_cast<float>( model_w_ ) / static_cast<float>( w_ );
    band_h_ = static_cast<int>( lroundf( static_cast<float>( h_ ) * s ) );

    if ( band_h_ > model_h_ )
        band_h_ = model_h_;
    band_top_ = ( model_h_ - band_h_ ) / 2;
    if ( band_top_ < 0 )
        band_top_ = 0;
    std::cout << "enc: tensor band y=[" << band_top_ << ',' << band_top_ + band_h_ << ") of " << model_w_ << 'x' << model_h_ << " -> " << w_
              << 'x' << h_ << '\n';

    // Two NV12 dma-bufs from an MPP DRM pool: ping-pong so a plane held by
    // the encoder is never overwritten by RGA on the next frame.
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
    ctx->time_base = AVRational { 1, fps_ };
    ctx->framerate = AVRational { fps_, 1 };
    ctx->bit_rate = static_cast<int64_t>( bitrate_kbps_ ) * 1000;
    ctx->gop_size = fps_;   // 1 s: a late joiner waits at most one GOP
    ctx->max_b_frames = 0;  // no reorder: minimal latency
    // Non-fatal: option names differ between ffmpeg-rockchip builds.
    if ( av_opt_set( ctx, "rc_mode", "CBR", 0 ) < 0 )
    {
        std::cout << "enc: rc_mode option not accepted, encoder default stays\n";
    }
    if ( avcodec_open2( ctx, codec, nullptr ) < 0 )
    {
        throw std::runtime_error( "net encoder: avcodec_open2(h264_rkmpp) failed" );
    }
    std::cout << "enc: h264_rkmpp open: " << w_ << 'x' << h_ << ", " << bitrate_kbps_ << " kbps, gop " << ctx->gop_size << '\n';

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
    if ( thread_.joinable() )
        thread_.join();
    if ( enc_ )
    {
        // Drain the encoder: send EOF and collect any pending packets so
        // the rkmpp wrapper releases its queued MppFrames while its own
        // MPP context is still alive. Without the flush the wrapper tears
        // down its pools first and then runs mpp_frame_deinit on the
        // leftovers — "non-positive ref_count" / "invalid mem pool ptr"
        // at exit.
        auto* ctx = static_cast<AVCodecContext*>( enc_ );
        // Drain only if frames were actually encoded: EOF on an idle
        // encoder makes the wrapper's flush path touch an already-torn-down
        // MPP pool ("invalid mem pool ptr" at exit, idle-run case).
        if ( pts_ > 0 && avcodec_send_frame( ctx, nullptr ) == 0 )
        {
            AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
            for ( ;; )
            {
                const int r = avcodec_receive_packet( ctx, pkt );
                if ( r == 0 )
                {
                    av_packet_unref( pkt );
                    continue;
                }
                break;  // EAGAIN / EOF / error — nothing left to drain
            }
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
    const std::string url = tcp_pull ? std::string() : ( "udp://" + host_ + ':' + std::to_string( port_ ) + "?pkt_size=1316" );

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
        std::cout << "enc: PULL mode: listening on tcp port " << port_ << " (viewer: ffplay -fflags nobuffer tcp://<board-ip>:" << port_
                  << ")\n";
    }

    while ( !quit_.load() )
    {
        error_.store( false );

        AVFormatContext* fc = nullptr;
        if ( avformat_alloc_output_context2( &fc, nullptr, "mpegts", tcp_pull ? nullptr : url.c_str() ) < 0 || !fc )
        {
            std::cerr << "enc: avformat_alloc_output_context2 failed\n";
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            continue;
        }

        AVIOContext* pb = nullptr;
        uint8_t* iobuf = nullptr;
        int cfd = -1;

        auto close_transport = [&]() {
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
        };

        if ( tcp_pull )
        {
            // Ждём зрителя; poll не даёт циклу крутиться вхолостую и не
            // блокирует выход по quit_.
            pollfd pf {};
            pf.fd = lfd;
            pf.events = POLLIN;

            while ( !quit_.load() && ::poll( &pf, 1, 200 ) == 0 )
            {}

            if ( quit_.load() )
            {
                avformat_free_context( fc );
                break;
            }

            cfd = ::accept4( lfd, nullptr, nullptr, SOCK_CLOEXEC );
            if ( cfd < 0 )
            {
                avformat_free_context( fc );
                continue;
            }

            int one = 1;
            ::setsockopt( cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
            const int sndbuf = 262144;  // запас на микроподлагивания сети
            ::setsockopt( cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof( sndbuf ) );
            const timeval tv { 2, 0 };  // зависший зритель отваливается, а не морозит конвейер
            ::setsockopt( cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
            std::cout << "enc: viewer connected (tcp)\n";

            iobuf = static_cast<uint8_t*>( av_malloc( 32768 ) );
            pb = iobuf ? avio_alloc_context( iobuf, 32768, 1, &cfd, nullptr, tcp_write_cb, nullptr ) : nullptr;
            if ( !pb )
            {
                if ( iobuf )
                    av_free( iobuf );
                ::close( cfd );
                avformat_free_context( fc );
                continue;
            }
            fc->pb = pb;
            fc->flags |= AVFMT_FLAG_CUSTOM_IO;
        }
        else
        {
            // UDP: без рукопожатия и слушателя — сокет открывается один
            // раз, пакеты пушатся вне зависимости от зрителя.
            const int ret = avio_open( &pb, url.c_str(), AVIO_FLAG_WRITE );
            if ( ret < 0 )
            {
                char err[64];
                av_strerror( ret, err, sizeof( err ) );
                std::cerr << "enc: udp open failed: " << err << '\n';
                avformat_free_context( fc );
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
                continue;
            }
            fc->pb = pb;
        }

        AVStream* st = avformat_new_stream( fc, nullptr );
        if ( !st )
        {
            close_transport();
            avformat_free_context( fc );
            continue;
        }
        if ( !st->codecpar )
        {
            st->codecpar = avcodec_parameters_alloc();
            if ( !st->codecpar )
            {
                close_transport();
                avformat_free_context( fc );
                continue;
            }
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264;
        st->codecpar->format = AV_PIX_FMT_NV12;
        st->codecpar->width = w_;
        st->codecpar->height = h_;
        st->codecpar->bit_rate = static_cast<AVCodecContext*>( enc_ )->bit_rate;
        st->time_base = AVRational { 1, fps_ };
        st->avg_frame_rate = AVRational { fps_, 1 };

        // Live-stream patch: повторять PAT/PMT/SDT в эфире, чтобы приёмник
        // мог подключиться в любой момент, а не только на старте потока.
        AVDictionary* mux_opts = nullptr;
        av_dict_set( &mux_opts, "mpegts_flags", "+pat_pmt_at_frames", 0 );
        av_dict_set( &mux_opts, "pat_period", "0.1", 0 );
        av_dict_set( &mux_opts, "sdt_period", "0.25", 0 );
        int ret2 = avformat_write_header( fc, &mux_opts );
        av_dict_free( &mux_opts );
        if ( ret2 < 0 )
        {
            char err[64];
            av_strerror( ret2, err, sizeof( err ) );
            std::cerr << "enc: write_header failed: " << err << '\n';
            close_transport();
            avformat_free_context( fc );
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            continue;
        }
        const AVRational tb = st->time_base;
        frame_step_ = av_rescale_q( 1, AVRational { 1, fps_ }, tb );
        if ( frame_step_ < 1 )
            frame_step_ = 1;
        std::cout << "enc: stream time_base " << tb.num << '/' << tb.den << ", pts step " << frame_step_ << "\n";
        if ( tcp_pull )
        {
            std::cout << "enc: tcp pull streaming " << w_ << 'x' << h_ << " h264 (mpegts) on port " << port_ << '\n';
        }
        else
        {
            std::cout << "enc: udp streaming " << w_ << 'x' << h_ << " h264 (mpegts) -> " << host_ << ':' << port_ << '\n';
        }

        {
            std::lock_guard<std::mutex> lk( mx_ );
            fc_ = fc;
            st_ = st;
            pts_ = 0;
        }

        // Park until quit or a write error; UDP writes do not block on a
        // dead viewer, TCP drops the stalled one via SO_SNDTIMEO.
        {
            std::unique_lock<std::mutex> lk( mx_ );
            cv_.wait( lk, [this] {
                return quit_.load() || error_.load();
            } );
        }
        {
            std::lock_guard<std::mutex> lk( mx_ );
            fc_ = nullptr;
            st_ = nullptr;
        }
        av_write_trailer( fc );  // best effort
        close_transport();
        avformat_free_context( fc );
        if ( !quit_.load() )
        {
            std::cout << ( tcp_pull ? "enc: viewer gone, waiting for the next one\n" : "enc: udp path reset, reopening in 500 ms\n" );
            if ( !tcp_pull )
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        }
    }
    if ( lfd >= 0 )
        ::close( lfd );
}

void NetEncoder::publish( int tensor_fd, int acquire_fence_fd )
{
    if ( quit_.load() )
    {
        if ( acquire_fence_fd >= 0 )
            ::close( acquire_fence_fd );
        return;
    }

    std::lock_guard<std::mutex> lk( mx_ );

    if ( !fc_ || !enc_ )
    {
        if ( acquire_fence_fd >= 0 )
            ::close( acquire_fence_fd );
        return;
    }

    if ( acq_fd_ >= 0 )
    {
        ::close( acq_fd_ );
        acq_fd_ = -1;
    }                            // stale fence
    tensor_fd_ = tensor_fd;      // RGA source for this frame
    acq_fd_ = acquire_fence_fd;  // consumed by send_frame
    if ( !send_frame( fc_, st_ ) )
    {
        error_.store( true );
        cv_.notify_all();  // serve() closes and reopens the UDP path
    }
}

bool NetEncoder::send_frame( void* fcp, void* stp )
{
    auto* fc = static_cast<AVFormatContext*>( fcp );
    auto* st = static_cast<AVStream*>( stp );
    auto* ctx = static_cast<AVCodecContext*>( enc_ );

    // Ping-pong: RGA never writes into a plane the previous encode still
    // holds. improcess is the synchronous single-op form: it returns
    // with the dst already finished for the CPU-side encoder read.
    // Ordering vs the GPU drawer is guaranteed upstream by poll() on
    // the exported draw fence (acq_fd_).
    cur_ ^= 1u;
    MppBuffer mb = bufs_[cur_];

    // Tensor path: crop the visible letterbox band out of the RGB888
    // model tensor and convert it to NV12 at 1:1 (no upscale). The
    // boxes are already in the band (GPU drawer + sync_cpu before
    // publish), so they ride along into the stream for free.
    rga_buffer_t src = wrapbuffer_fd( tensor_fd_, model_w_, model_h_, RK_FORMAT_RGB_888 );
    im_rect srect = { 0, 0, 0, 0 };
    srect.x = 0;
    srect.y = band_top_;
    srect.width = model_w_;
    srect.height = band_h_;
    // Explicit strides: the VU plane lands at w * vstride_ - exactly where
    // the DRM descriptor below reports it (the default tight h would put
    // chroma where the encoder cfg does not look).
    rga_buffer_t dst = wrapbuffer_fd( mpp_buffer_get_fd( mb ), w_, h_, RK_FORMAT_YCbCr_420_SP, w_, vstride_ );

    // RGA-fence support is absent in this kernel driver: the acquire
    // fence passed to improcess was silently ignored (boxes vanished).
    // The draw fence is a plain sync_file fd: wait via poll() — much
    // cheaper than sync_cpu(), same ordering guarantee for RGA.
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
    const IM_STATUS rs = improcess( src, dst, {}, srect, {}, {}, 0 );
    if ( (int)rs <= 0 )
    {
        std::cerr << "enc: RGA convert failed: " << imStrError( rs ) << '\n';
        return false;
    }

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
    fr->pts = static_cast<int64_t>( pts_ );
    fr->data[0] = reinterpret_cast<uint8_t*>( &desc );
    fr->buf[0] = av_buffer_create( reinterpret_cast<uint8_t*>( &desc ), sizeof( desc ), noop_free, nullptr, 0 );
    if ( !fr->buf[0] )
    {
        std::cerr << "enc: av_buffer_create failed\n";
        return false;
    }

    int ret = avcodec_send_frame( ctx, fr );
    av_frame_unref( fr );  // drop the buffer ref; the tensor/MPP memory stays
    if ( ret < 0 )
    {
        char err[64];
        av_strerror( ret, err, sizeof( err ) );
        std::cerr << "enc: send_frame: " << err << '\n';
        return false;
    }

    AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
    for ( ;; )
    {
        ret = avcodec_receive_packet( ctx, pkt );
        if ( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
            break;
        if ( ret < 0 )
        {
            char err[64];
            av_strerror( ret, err, sizeof( err ) );
            std::cerr << "enc: receive_packet: " << err << '\n';
            return false;
        }
        pkt->stream_index = st->index;
        pkt->pts = pkt->dts = static_cast<int64_t>( pts_ * frame_step_ );
        const int r = av_interleaved_write_frame( fc, pkt );
        av_packet_unref( pkt );
        if ( r < 0 )
        {
            char err[64];
            av_strerror( r, err, sizeof( err ) );
            std::cerr << "enc: write failed: " << err << '\n';
            return false;
        }
    }
    ++pts_;
    return true;
}
