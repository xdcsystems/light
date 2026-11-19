#include <chrono>
#include <cstddef>
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

    size_t frame_byte_size( int h, int stride ) noexcept
    {
        if ( h <= 0 || stride <= 0 )
            return 0;
        return static_cast<size_t>( stride ) * static_cast<size_t>( h );
    }
}  // namespace

NetPreview::NetPreview( int w, int h, int stride, int fps, int port, const Log& log )
  : log_( log )
  , w_( w )
  , h_( h )
  , fps_( fps > 0 ? fps : 25 )
  , pace_fps_( fps > 0 ? fps : 0 )
  , port_( port )
  , ring_( frame_byte_size( h, stride ) )
  , send_buf_( frame_byte_size( h, stride ) )
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
        const std::lock_guard<std::mutex> lk( tcp_mx_ );
        if ( active_tcp_ != nullptr )
            active_tcp_->abort_client();
    }
    ring_.stop();
    if ( thread_.joinable() )
        thread_.join();
    if ( pkt_ )
        av_packet_free( &pkt_ );
    avformat_network_deinit();
}

void NetPreview::serve() noexcept
{
    TcpPullServer tcp( port_, 2 * 1024 * 1024, 5 );
    struct TcpReg
    {
        std::mutex& mx;
        TcpPullServer*& slot;
        TcpReg( std::mutex& m, TcpPullServer*& s, TcpPullServer& t ) : mx( m ), slot( s )
        {
            const std::lock_guard<std::mutex> lk( mx );
            slot = &t;
        }
        ~TcpReg()
        {
            const std::lock_guard<std::mutex> lk( mx );
            slot = nullptr;
        }
    } tcp_reg( tcp_mx_, active_tcp_, tcp );
    // The bind retries until it succeeds or quit ends the thread; the run
    // keeps going without a viewer.
    if ( Status st = retry_listen( [&] { return tcp.listen(); }, quit_, log_, "net", port_ ); !st.ok() )
        return;
    log_.info() << "net: listening on tcp port " << port_ << " (viewer: ffplay tcp://<board-ip>:" << port_ << ")\n";

    while ( !quit_.load() )
    {
        if ( Status ast = tcp.accept_client( quit_ ); !ast.ok() )
            continue;

        NutSessionGuard s( tcp );
        AVStream* st = nullptr;
        if ( Status ost = open_session( tcp, s.fc, st ); !ost.ok() )
        {
            log_.error() << "net: " << ost << '\n';
            continue;
        }
        const uint64_t frame_step = compute_frame_step( st );
        go_live();
        const bool write_err = drain_session( s.fc, st, frame_step );
        close_session( s.fc, write_err );
    }
}

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

uint64_t NetPreview::compute_frame_step( const AVStream* st ) const noexcept
{
    const AVRational tb = st->time_base;
    int64_t step = av_rescale_q( 1, AVRational { 1, fps_ }, tb );
    if ( step < 1 )
        step = 1;
    log_.info() << "net: stream time_base " << tb.num << '/' << tb.den << ", pts step " << step << "\n";
    log_.info() << "net: viewer connected, streaming " << w_ << 'x' << h_ << " rgb24 (nut) on port " << port_ << '\n';
    return static_cast<uint64_t>( step );
}

void NetPreview::go_live() noexcept
{
    next_send_us_.store( 0, std::memory_order_relaxed );
    ring_.open();
    session_epoch_.fetch_add( 1, std::memory_order_acq_rel );
    connected_.store( true, std::memory_order_release );
}

bool NetPreview::session_open() const noexcept
{
    return connected_.load( std::memory_order_acquire );
}

uint32_t NetPreview::session_epoch() const noexcept
{
    return session_epoch_.load( std::memory_order_acquire );
}

bool NetPreview::drain_session( AVFormatContext* fc, AVStream* st, uint64_t frame_step ) noexcept
{
    uint64_t pts = 0;
    while ( !quit_.load( std::memory_order_relaxed ) && ring_.pop( send_buf_ ) )
    {
        pkt_->buf = nullptr;
        pkt_->data = send_buf_.data();
        pkt_->size = static_cast<int>( send_buf_.size() );
        pkt_->stream_index = st->index;
        pkt_->pts = pkt_->dts = static_cast<int64_t>( pts * frame_step );
        pkt_->duration = static_cast<int64_t>( frame_step );
        pkt_->flags |= AV_PKT_FLAG_KEY;
        ++pts;

        const int ret = av_write_frame( fc, pkt_ );
        pkt_->data = nullptr;
        pkt_->size = 0;
        if ( ret < 0 )
        {
            log_.warn() << "net: write failed: " << av_err_str( ret ) << '\n';
            return true;
        }
        avio_flush( fc->pb );
        if ( fc->pb && fc->pb->error < 0 )
        {
            log_.warn() << "net: flush failed: " << av_err_str( fc->pb->error ) << '\n';
            return true;
        }
    }
    return false;
}

void NetPreview::close_session( AVFormatContext* fc, bool write_err )
{
    connected_.store( false, std::memory_order_release );
    ring_.close();
    if ( !write_err )
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
    if ( !connected_.load( std::memory_order_acquire ) || quit_.load( std::memory_order_relaxed ) || rgb == nullptr )
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
    static_cast<void>( ring_.push( rgb ) );
}
