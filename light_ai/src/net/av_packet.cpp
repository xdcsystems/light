#include <stdexcept>

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include "net/av_packet.hpp"

AvPacket::AvPacket() : p_( av_packet_alloc() )
{
    if ( !p_ )
        throw std::runtime_error( "net encoder: av_packet_alloc failed" );
}

AvPacket::~AvPacket()
{
    av_packet_free( &p_ );
}
