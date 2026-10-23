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
    // w/h come from Config (validated against the camera G_FMT) instead of
    // being hard-coded: the pre-allocated output buffer must match the
    // actual stream size, otherwise a resolution change either triggers
    // info_change churn or overruns the buffer.
    MppDecoder(int w, int h);
    ~MppDecoder();

    int decode(const uint8_t* data, size_t size);

private:
    MppCtx         ctx_ = nullptr;
    MppApi*        mpi_ = nullptr;
    MppBufferGroup out_group_ = nullptr;
    MppFrame       out_frame_ = nullptr;
    MppPacket      pkt_ = nullptr;
    MppBufferGroup pkt_group_ = nullptr;

    int            w_ = 0, h_ = 0;
    uint64_t       pts_ = 0;
    int            frames_ = 0;
    bool           ok_ = false;
};
