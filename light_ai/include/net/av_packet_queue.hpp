// PacketQueue: BasicPacketQueue (core/packet_queue.hpp) over AVPacket*.
// Each slot is an av_packet_alloc'ed AVPacket struct for the lifetime of
// the queue; payloads move in and out with av_packet_move_ref (no heap
// per packet). A key frame is an IDR access unit (annexb::is_idr).

#pragma once

#include "core/packet_queue.hpp"
#include "core/session_gate.hpp"
#include "platform/pi_mutex.hpp"

struct AVPacket;

struct AvPacketOps
{
    bool init( AVPacket*& p );
    void release( AVPacket*& p );
    void unref( AVPacket*& p );
    void move( AVPacket*& dst, AVPacket*& src );
    bool is_key( AVPacket* const& p ) const;
};

using PacketQueue = BasicPacketQueue<AVPacket*, AvPacketOps, 32, light_ai::platform::PiMutex, light_ai::platform::PiCondVar>;
using NetSessionGate = BasicSessionGate<light_ai::platform::PiMutex>;
