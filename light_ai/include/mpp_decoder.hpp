#pragma once
#include <cstddef>
#include <cstdint>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_meta.h>

class MppDecoder {
public:
    MppDecoder();
    ~MppDecoder();

    int decode(const uint8_t* data, size_t size);

private:
    MppCtx         ctx_ = nullptr;
    MppApi*        mpi_ = nullptr;
    MppBufferGroup out_group_ = nullptr;
    MppFrame       out_frame_ = nullptr;
    MppPacket      pkt_ = nullptr;      
    MppBufferGroup pkt_group_ = nullptr;

    uint64_t       pts_ = 0;
    bool           ok_ = false;
};
