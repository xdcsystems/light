#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/codec_par.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

#include "net_preview.hpp"

namespace
{
    // The packet references the NPU tensor memory; unref must never free it.
    void noop_free( void*, uint8_t* ) {}

}  // namespace

NetPreview::NetPreview( int w, int h, int stride, int fps, int port )
  : w_( w ), h_( h ), stride_( stride ), fps_( fps > 0 ? fps : 25 ), port_( port )
{
    avformat_network_init();
    pkt_ = av_packet_alloc();
    thread_ = std::thread( &NetPreview::serve, this );
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
    {
        AVPacket* pkt = static_cast<AVPacket*>( pkt_ );
        av_packet_free( &pkt );
        pkt_ = nullptr;
    }
    avformat_network_deinit();
}

// ffmpeg AVIO interrupt callback (statics rule): file-local free
// function; the opaque context carries the quit flag pointer, not
// the object, so no private access is needed.
namespace
{
    int preview_interrupt_cb( void* arg ) { return static_cast<std::atomic<bool>*>( arg )->load() ? 1 : 0; }
}  // anonymous namespace

void NetPreview::serve()
{
    std::ostringstream os;
    os << "tcp://0.0.0.0:" << port_;
    const std::string url = os.str();

    while ( !quit_.load() )
    {
        error_.store( false );

        AVFormatContext* fc = nullptr;
        if ( avformat_alloc_output_context2( &fc, nullptr, "nut", url.c_str() ) < 0 || !fc )
        {
            std::cerr << "net: avformat_alloc_output_context2 failed\n";
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            continue;
        }

        AVDictionary* opts = nullptr;
        av_dict_set( &opts, "listen", "1", 0 );
        // Match the documented contract: a stalled viewer must not stall
        // the pipeline. 5 s in microseconds; also bounds the blocking
        // accept — a failed open just retries in 500 ms.
        av_dict_set( &opts, "rw_timeout", "5000000", 0 );
        AVIOInterruptCB cb = { &preview_interrupt_cb, &quit_ };
        AVIOContext* pb = nullptr;
        // Blocks here until a viewer connects (or the interrupt fires on quit).
        const int ret = avio_open2( &pb, url.c_str(), AVIO_FLAG_WRITE, &cb, &opts );
        av_dict_free( &opts );
        if ( ret < 0 )
        {
            char err[64];
            av_strerror( ret, err, sizeof( err ) );
            std::cerr << "net: waiting for viewer: " << err << '\n';
            avformat_free_context( fc );
            if ( quit_.load() )
                break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            continue;
        }
        fc->pb = pb;

        AVStream* st = avformat_new_stream( fc, nullptr );
        if ( !st )
        {
            avio_closep( &pb );
            avformat_free_context( fc );
            continue;
        }
        // Newer libavformat branches do not allocate codecpar inside
        // avformat_new_stream: it must be allocated explicitly. It stays
        // owned by the stream (avformat_free_context frees it).
        if ( !st->codecpar )
        {
            st->codecpar = avcodec_parameters_alloc();
            if ( !st->codecpar )
            {
                std::cerr << "net: avcodec_parameters_alloc failed\n";
                avio_closep( &pb );
                avformat_free_context( fc );
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
        // NUT requires an explicit fourcc for rawvideo: "RGB[24]" is the
        // canonical NUT tag for raw rgb24 (0x18424752). Without it the
        // muxer rejects write_header with "No codec tag defined".
        st->codecpar->codec_tag = MKTAG( 'R', 'G', 'B', 24 );

        // Diagnostic: the tag must show 0x18424752 (RGB[24]). If it shows
        // 0 here, we are running a stale binary or the tag got wiped.
        std::cout << "net: write_header attempt, codec_tag=0x" << std::hex << st->codecpar->codec_tag << std::dec << "\n";
        int ret2 = avformat_write_header( fc, nullptr );
        if ( ret2 < 0 )
        {
            char err[64];
            av_strerror( ret2, err, sizeof( err ) );
            std::cerr << "net: write_header failed: " << err << '\n';
            avio_closep( &pb );
            avformat_free_context( fc );
            if ( quit_.load() )
                break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
            continue;
        }
        // avformat_write_header may rewrite st->time_base (the NUT muxer
        // picks its own, e.g. 1/25 -> 1/51200). All pts written afterwards
        // must be expressed in the NEW time base, otherwise the viewer sees
        // the whole stream compressed into milliseconds and freezes.
        const AVRational tb = st->time_base;
        frame_step_ = av_rescale_q( 1, AVRational { 1, fps_ }, tb );
        if ( frame_step_ < 1 )
            frame_step_ = 1;
        std::cout << "net: stream time_base " << tb.num << '/' << tb.den << ", pts step " << frame_step_ << "\n";
        std::cout << "net: viewer connected, streaming " << w_ << 'x' << h_ << " rgb24 (nut) on port " << port_ << '\n';

        {
            std::lock_guard<std::mutex> lk( mx_ );
            fc_ = fc;
            stream_ = st;
            pts_ = 0;
        }
        connected_.store( true, std::memory_order_release );

        // Idle until the publisher signals error or the pipeline quits.
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
        av_write_trailer( fc );  // best effort; on a dead link it just fails
        avio_closep( &pb );
        avformat_free_context( fc );
        if ( !quit_.load() )
            std::cout << "net: viewer gone, waiting for a new one\n";
    }
}

void NetPreview::publish( const uint8_t* rgb )
{
    if ( !connected_.load( std::memory_order_acquire ) )
        return;
    std::lock_guard<std::mutex> lk( mx_ );  // held for the write: protects fc_
    if ( !fc_ || quit_.load() )
        return;
    auto* fc = static_cast<AVFormatContext*>( fc_ );
    auto* st = static_cast<AVStream*>( stream_ );
    auto* p = static_cast<AVPacket*>( pkt_ );

    // Zero-copy: data points into the NPU input tensor (dma-buf).
    AVBufferRef* ref = av_buffer_create( const_cast<uint8_t*>( rgb ), stride_ * h_, noop_free, nullptr, 0 );
    if ( !ref )
        return;
    p->buf = ref;  // packet takes over the reference
    p->data = ref->data;
    p->size = stride_ * h_;
    p->stream_index = st->index;
    p->pts = p->dts = static_cast<int64_t>( pts_ * frame_step_ );
    p->duration = frame_step_;  // one frame, in the muxer's time base
    p->flags |= AV_PKT_FLAG_KEY;
    ++pts_;

    const int ret = av_write_frame( fc, p );
    av_packet_unref( p );  // drops the buffer ref (no-op free), tensor intact
    if ( ret < 0 )
    {
        char err[64];
        av_strerror( ret, err, sizeof( err ) );
        std::cerr << "net: write failed: " << err << '\n';
        error_.store( true );
        connected_.store( false, std::memory_order_release );
        cv_.notify_all();  // serve() closes and waits for a new viewer
        return;
    }
    avio_flush( fc->pb );  // push the frame out immediately (low latency)
}
