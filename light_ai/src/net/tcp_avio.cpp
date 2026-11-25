#include <cerrno>
#include <cstdint>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include "net/tcp_avio.hpp"
#include "platform/tcp_pull_server.hpp"

namespace
{
    enum
    {
        kIoBufBytes = 174 * 188  // exact multiple of 188-byte MPEG-TS packet (32712 B)
    };

    // The AVIO write callback: the POSIX drain loop is tcp_send_chunk()
    // (src/platform/tcp_send.cpp, host-tested); its negative codes are -errno, which
    // is exactly FFmpeg's AVERROR() on POSIX, except the peer-EOF marker.
    // NOLINTNEXTLINE(misc-redundant-expression): intentional drift guard - AVERROR(e) expands to -(e) on POSIX
    static_assert( AVERROR( EPIPE ) == -EPIPE && AVERROR( ETIMEDOUT ) == -ETIMEDOUT && AVERROR( EIO ) == -EIO,
                   "AVERROR is not -errno on this platform" );
    // NOLINTNEXTLINE(misc-const-correctness): avio_alloc_context fixes the callback type, opaque is void*
    int write_packet( void* opaque, const uint8_t* buf, int buf_size )
    {
        const int r = tcp_send_chunk( *static_cast<const TcpPullServer::WriteCtx*>( opaque ), buf, buf_size );
        return r == kTcpSendEof ? AVERROR_EOF : r;
    }
}  // namespace

Status TcpAvio::attach_avio( TcpPullServer& tcp, AVFormatContext* fc )
{
    close();
    if ( !tcp.has_client() || !fc )
        return make_status( Errc::Invalid, "no client" );
    iobuf_ = static_cast<uint8_t*>( av_malloc( kIoBufBytes ) );
    if ( !iobuf_ )
        return make_status( Errc::NoMemory, "avio buffer alloc failed" );
    pb_ = avio_alloc_context( iobuf_, kIoBufBytes, 1, &tcp.write_ctx(), nullptr, write_packet, nullptr );
    if ( !pb_ )
    {
        av_freep( &iobuf_ );
        return make_status( Errc::NoMemory, "avio_alloc_context failed" );
    }
    fc->pb = pb_;
    fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    return {};
}

void TcpAvio::close() noexcept
{
    if ( pb_ )
    {
        av_freep( &pb_->buffer );
        avio_context_free( &pb_ );
        iobuf_ = nullptr;  // the context owned it (or its replacement): freed above
    }
    if ( iobuf_ )  // allocated, but no context took it over
        av_freep( &iobuf_ );
}
