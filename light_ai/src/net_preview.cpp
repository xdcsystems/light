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
#include "muxer_teardown.hpp"
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
            teardown_muxer( fc );  // shared with NetEncoder::FcGuard
            tcp.close_client();
        }
        explicit NutSessionGuard( TcpPullServer& s ) : tcp( s ) {}
        NutSessionGuard( const NutSessionGuard& ) = delete;
        NutSessionGuard& operator=( const NutSessionGuard& ) = delete;
    };

    int64_t now_steady_us()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch() )
            .count();
    }
}  // namespace

NetPreview::NetPreview( int w, int h, int stride, int fps, int port )
  : w_( w ), h_( h ), stride_( stride ), fps_( fps > 0 ? fps : 25 ), pace_fps_( fps > 0 ? fps : 0 ), port_( port )
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
        if ( !st || !st->codecpar )
            continue;
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
            next_send_us_.store( 0, std::memory_order_relaxed );
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

bool NetPreview::viewer_connected() const
{
    if ( !connected_.load( std::memory_order_acquire ) )
        return false;
    if ( pace_fps_ <= 0 )
        return true;
    const int64_t step_us = 1000000 / pace_fps_;
    const int64_t next_us = next_send_us_.load( std::memory_order_relaxed );
    return next_us == 0 || now_steady_us() + step_us / 8 >= next_us;
}

void NetPreview::publish( const uint8_t* rgb )
{
    if ( !connected_.load( std::memory_order_acquire ) )
        return;
    std::lock_guard<std::mutex> lk( mx_ );
    if ( !fc_ || quit_.load() )
        return;
    if ( pace_fps_ > 0 )
    {
        const int64_t step_us = 1000000 / pace_fps_;
        const int64_t now_us = now_steady_us();
        const int64_t next_us = next_send_us_.load( std::memory_order_relaxed );
        if ( next_us > 0 && now_us + step_us / 8 < next_us )
            return;
        const int64_t target = next_us > 0 ? next_us + step_us : now_us + step_us;
        next_send_us_.store( target > now_us ? target : now_us + step_us, std::memory_order_relaxed );
    }

    // Zero per-frame heap allocation: av_write_frame synchronously muxes
    // pkt_->data into the NUT AVIO stream without taking buffer ownership.
    pkt_->buf = nullptr;
    pkt_->data = const_cast<uint8_t*>( rgb );  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    pkt_->size = stride_ * h_;
    pkt_->stream_index = stream_->index;
    pkt_->pts = pkt_->dts = static_cast<int64_t>( pts_ * frame_step_ );
    pkt_->duration = frame_step_;
    pkt_->flags |= AV_PKT_FLAG_KEY;
    ++pts_;

    const int ret = av_write_frame( fc_, pkt_ );
    pkt_->data = nullptr;
    pkt_->size = 0;
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
