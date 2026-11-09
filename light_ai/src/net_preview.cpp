#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

extern "C"
{
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

#include "av_common.hpp"
#include "net_preview.hpp"
#include "tcp_pull_server.hpp"

namespace
{
    struct NutSessionGuard
    {
        TcpPullServer& tcp;
        AVFormatContext* fc = nullptr;

        ~NutSessionGuard()
        {
            if ( fc )
            {
                fc->pb = nullptr;
                avformat_free_context( fc );
            }
            tcp.close_client();
        }
        explicit NutSessionGuard( TcpPullServer& s ) : tcp( s ) {}
        NutSessionGuard( const NutSessionGuard& ) = delete;
        NutSessionGuard& operator=( const NutSessionGuard& ) = delete;
    };
}  // namespace

NetPreview::NetPreview( int w, int h, int stride, int fps, int port )
  : w_( w ), h_( h ), stride_( stride ), fps_( fps > 0 ? fps : 25 ), port_( port )
{
    avformat_network_init();
    pkt_ = av_packet_alloc();
    if ( !pkt_ )
    {
        avformat_network_deinit();
        throw std::runtime_error( "net_preview: av_packet_alloc failed" );
    }
    try
    {
        thread_ = std::thread( &NetPreview::serve, this );
    }
    catch ( ... )
    {
        av_packet_free( &pkt_ );
        avformat_network_deinit();
        throw;
    }
}

NetPreview::~NetPreview()
{
    quit_.store( true );
    {
        std::lock_guard<std::mutex> lk( mx_ );
        cv_.notify_all();
    }
    if ( thread_.joinable() )
        thread_.join();
    if ( pkt_ )
        av_packet_free( &pkt_ );
    avformat_network_deinit();
}

void NetPreview::serve()
{
    TcpPullServer tcp( port_, 2 * 1024 * 1024, 5 );
    if ( !tcp.listen() )
    {
        std::cerr << "net: tcp listener bind failed on port " << port_ << '\n';
        error_.store( true );
        return;
    }
    std::cout << "net: listening on tcp port " << port_
              << " (viewer: ffplay tcp://<board-ip>:" << port_ << ")\n";

    while ( !quit_.load() )
    {
        error_.store( false );

        if ( !tcp.accept_client( quit_ ) )
            continue;

        NutSessionGuard s( tcp );
        if ( avformat_alloc_output_context2( &s.fc, nullptr, "nut", nullptr ) < 0 || !s.fc )
        {
            std::cerr << "net: avformat_alloc_output_context2 failed\n";
            continue;
        }
        if ( !tcp.attach_avio( s.fc ) )
            continue;

        AVStream* st = avformat_new_stream( s.fc, nullptr );
        if ( !st )
            continue;
        if ( !st->codecpar )
        {
            st->codecpar = avcodec_parameters_alloc();
            if ( !st->codecpar )
            {
                std::cerr << "net: avcodec_parameters_alloc failed\n";
                continue;
            }
        }
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->format = AV_PIX_FMT_RGB24;
        st->codecpar->width = w_;
        st->codecpar->height = h_;
        st->time_base = AVRational { 1, fps_ };
        st->avg_frame_rate = AVRational { fps_, 1 };
        st->codecpar->codec_tag = MKTAG( 'R', 'G', 'B', 24 );

        std::cout << "net: write_header attempt, codec_tag=0x" << std::hex << st->codecpar->codec_tag << std::dec << "\n";
        const int ret2 = avformat_write_header( s.fc, nullptr );
        if ( ret2 < 0 )
        {
            std::cerr << "net: write_header failed: " << av_err_str( ret2 ) << '\n';
            continue;
        }
        const AVRational tb = st->time_base;
        int64_t step = av_rescale_q( 1, AVRational { 1, fps_ }, tb );
        if ( step < 1 )
            step = 1;
        std::cout << "net: stream time_base " << tb.num << '/' << tb.den << ", pts step " << step << "\n";
        std::cout << "net: viewer connected, streaming " << w_ << 'x' << h_ << " rgb24 (nut) on port " << port_ << '\n';

        {
            std::lock_guard<std::mutex> lk( mx_ );
            fc_ = s.fc;
            stream_ = st;
            frame_step_ = static_cast<uint64_t>( step );
            pts_ = 0;
        }
        connected_.store( true, std::memory_order_release );

        {
            std::unique_lock<std::mutex> lk( mx_ );
            cv_.wait( lk, [this] {
                return quit_.load() || error_.load();
            } );
        }
        connected_.store( false, std::memory_order_release );
        {
            std::lock_guard<std::mutex> lk( mx_ );
            fc_ = nullptr;
            stream_ = nullptr;
        }
        av_write_trailer( s.fc );
        if ( !quit_.load() )
            std::cout << "net: viewer gone, waiting for a new one\n";
    }
}

void NetPreview::publish( const uint8_t* rgb )
{
    if ( !connected_.load( std::memory_order_acquire ) )
        return;
    std::lock_guard<std::mutex> lk( mx_ );
    if ( !fc_ || quit_.load() )
        return;

    AVBufferRef* ref = av_buffer_create( const_cast<uint8_t*>( rgb ), stride_ * h_, av_noop_free, nullptr, 0 ); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if ( !ref )
        return;
    pkt_->buf = ref;
    pkt_->data = ref->data;
    pkt_->size = stride_ * h_;
    pkt_->stream_index = stream_->index;
    pkt_->pts = pkt_->dts = static_cast<int64_t>( pts_ * frame_step_ );
    pkt_->duration = frame_step_;
    pkt_->flags |= AV_PKT_FLAG_KEY;
    ++pts_;

    const int ret = av_write_frame( fc_, pkt_ );
    av_packet_unref( pkt_ );
    if ( ret < 0 )
    {
        std::cerr << "net: write failed: " << av_err_str( ret ) << '\n';
        error_.store( true );
        connected_.store( false, std::memory_order_release );
        cv_.notify_all();
        return;
    }
    avio_flush( fc_->pb );
}
