// EncoderFramePool: the ring of NV12 dma-bufs the H.264 encoder reads, each
// pre-bound to an AVFrame as an AV_PIX_FMT_DRM_PRIME descriptor. The rkmpp
// wrapper imports the dma-buf by fd: no CPU pixel copies anywhere in the path.
//
// Eight slots: an 8-slot ring stays well ahead of RGA writes and VEPU / V4L2
// async pipeline depth (even alongside AV_CODEC_FLAG_LOW_DELAY output draining).
//
// RAII: the ctor allocates everything or throws std::runtime_error (fail
// loud at startup, nothing leaks); the dtor frees the AVFrames, then the
// dma-bufs (RkmppDrmPool member). The codec that references the frames must
// be gone first: the owner declares the pool before the codec.
// Thread: the pipeline thread (publish) only.

#pragma once

#include <array>

// DRM frame descriptor (the TYPE only; the DRM_FORMAT_* constants are the cpp's business).
#include <libavutil/hwcontext_drm.h>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers.
#include <rockchip/rk_mpi.h>

#include "core/encoder_config.hpp"
#include "core/non_copyable.hpp"
#include "media/rkmpp_drm_pool.hpp"

struct AVFrame;

class EncoderFramePool : private NonMovable<EncoderFramePool>
{
public:
    enum
    {
        kSlots = 8
    };

public:
    explicit EncoderFramePool( const StreamLayout& layout );
    ~EncoderFramePool();

    // Steps the ring to the next writable slot (not referenced by the VPU) or returns -1.
    int acquire_writable() noexcept;
    int current() const { return cur_; }
    MppBuffer buf( int slot ) const { return ring_.buf[slot]; }
    AVFrame* frame( int slot ) const { return frames_[slot]; }

private:
    void release();

private:
    RkmppDrmPool<kSlots> ring_;
    std::array<AVDRMFrameDescriptor, kSlots> desc_ {};
    std::array<AVFrame*, kSlots> frames_ {};
    int cur_ = 0;
};
