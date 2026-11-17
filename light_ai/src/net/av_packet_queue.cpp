#include <cstddef>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "core/param_set_cache.hpp"  // annexb::is_idr
#include "net/av_packet_queue.hpp"

bool AvPacketOps::init( AVPacket*& p )
{
    p = av_packet_alloc();
    return p != nullptr;
}

void AvPacketOps::release( AVPacket*& p )
{
    if ( p )
        av_packet_free( &p );
}

void AvPacketOps::unref( AVPacket*& p )
{
    av_packet_unref( p );
}

void AvPacketOps::move( AVPacket*& dst, AVPacket*& src )
{
    av_packet_unref( dst );
    av_packet_move_ref( dst, src );
}

bool AvPacketOps::is_key( AVPacket* const& p ) const
{
    return annexb::is_idr( p->data, static_cast<size_t>( p->size ) );
}
