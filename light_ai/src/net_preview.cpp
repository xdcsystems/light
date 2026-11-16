#include <chrono>
#include <cstring>
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
#include "core/log.hpp"
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

NetPreview::NetPreview( int w, int h, int stride, int fps, int port, const Log& log )
  : log_( log ), w_( w ), h_( h ), stride_( stride ), fps_( fps > 0 ? fps : 25 ), pace_fps_( fps > 0 ? fps : 0 ), port_( port )
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
    if ( Status st = tcp.listen(); !st.ok() )
    {
        log_.error() << "net: tcp listener on port " << port_ << ": " << st << '\n';
        error_.store( true );
        return;
    }
    log_.info() << "net: listening on tcp port " << port_ << " (viewer: ffplay tcp://<board-ip>:" << port_ << ")\n";

    while ( !quit_.load() )
    {
        error_.store( false );

        if ( Status ast = tcp.accept_client( quit_ ); !ast.ok() )
            continue;

        NutSessionGuard s( tcp );
        AVStream* st = nullptr;
        if ( Status ost = open_session( tcp, s.fc, st ); !ost.ok() )
        {
            log_.error() << "net: " << ost << '\n';
            continue;
        }
        go_live( s.fc, st );
        wait_session_end();
        close_session( s.fc );
    }
}

// The muxer of one viewer: a NUT context over the client socket, one raw
// RGB24 stream, the header written. The steps return their Status; the
// caller logs it ("net: <st>"), serve()'s guard frees fc and closes the
// client.
Status NetPreview::open_session( TcpPullServer& tcp, AVFormatContext*& fc, AVStream*& st )
{
    if ( const int r = avformat_alloc_output_context2( &fc, nullptr, "nut", nullptr ); r < 0 || !fc )
        return sdk_status( Errc::Failed, "avformat_alloc_output_context2 failed", r );
    if ( Status ast = tcp.attach_avio( fc ); !ast.ok() )
        return ast;

    st = avformat_new_stream( fc, nullptr );
    if ( !st || !st->codecpar )
        return make_status( Errc::NoMemory, "avformat_new_stream failed" );
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format = AV_PIX_FMT_RGB24;
    st->codecpar->width = w_;
    st->codecpar->height = h_;
    st->time_base = AVRational { 1, fps_ };
    st->avg_frame_rate = AVRational { fps_, 1 };
    st->codecpar->codec_tag = MKTAG( 'R', 'G', 'B', 24 );

    log_.info() << "net: write_header attempt, codec_tag=0x" << std::hex << st->codecpar->codec_tag << std::dec << "\n";
    if ( const int ret2 = avformat_write_header( fc, nullptr ); ret2 < 0 )
        return sdk_status( Errc::Failed, "write_header failed", ret2 );
    return {};
}

// The pts step in the time base the muxer chose, then the session goes
// public: publish() writes from the next pipeline frame on.
void NetPreview::go_live( AVFormatContext* fc, AVStream* st )
{
    const AVRational tb = st->time_base;
    int64_t step = av_rescale_q( 1, AVRational { 1, fps_ }, tb );
    if ( step < 1 )
        step = 1;
    log_.info() << "net: stream time_base " << tb.num << '/' << tb.den << ", pts step " << step << "\n";
    log_.info() << "net: viewer connected, streaming " << w_ << 'x' << h_ << " rgb24 (nut) on port " << port_ << '\n';

    {
        std::lock_guard<std::mutex> lk( mx_ );
        fc_ = fc;
        stream_ = st;
        frame_step_ = static_cast<uint64_t>( step );
        pts_ = 0;
        next_send_us_.store( 0, std::memory_order_relaxed );
    }
    connected_.store( true, std::memory_order_release );
}

// Until the destructor sets quit_ or publish() a write error.
void NetPreview::wait_session_end()
{
    std::unique_lock<std::mutex> lk( mx_ );
    cv_.wait( lk, [this] {
        return quit_.load() || error_.load();
    } );
}

// The session ends: publish() stops first (fc_ cleared under mx_, so no write
// is in flight), then the trailer -- only when no write failed (error_ unset).
void NetPreview::close_session( AVFormatContext* fc )
{
    connected_.store( false, std::memory_order_release );
    {
        std::lock_guard<std::mutex> lk( mx_ );
        fc_ = nullptr;
        stream_ = nullptr;
    }
    if ( !error_.load() )
        av_write_trailer( fc );
    if ( !quit_.load() )
        log_.info() << "net: viewer gone, waiting for a new one\n";
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
        log_.warn() << "net: write failed: " << av_err_str( ret ) << '\n';
        error_.store( true );
        connected_.store( false, std::memory_order_release );
        cv_.notify_all();
        return;
    }
    avio_flush( fc_->pb );
}
