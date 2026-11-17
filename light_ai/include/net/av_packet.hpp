// AvPacket: one av_packet_alloc'ed AVPacket struct, owned for the lifetime
// of the holder (RAII: the ctor allocates or throws std::runtime_error, the
// dtor frees). Payloads come and go with av_packet_move_ref / unref; the
// struct itself is never reallocated, so no heap per packet.

#pragma once

struct AVPacket;

class AvPacket
{
public:
    AvPacket();
    ~AvPacket();
    AvPacket( const AvPacket& ) = delete;
    AvPacket& operator=( const AvPacket& ) = delete;

    AVPacket* get() const { return p_; }
    // For PacketQueue::pop( Pkt& ): the queue moves a payload into the
    // struct; it never replaces the pointer.
    AVPacket*& slot() { return p_; }

private:
    AVPacket* p_ = nullptr;
};
