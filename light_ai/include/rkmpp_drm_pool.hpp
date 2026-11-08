// MPP DRM pool (P1-4) as one RAII sub-object (2026-11-07): the buffer
// group plus its N NV12 dma-bufs, fixed capacity on std::array (no heap).
// Rationale: the former stepwise members (buf_group_ + bufs_) leaked the
// whole pool whenever the owning constructor threw after the pool
// allocation (avcodec_open2 / av_frame_alloc paths). A fully constructed
// sub-object is destroyed even when the owner's constructor throws, so
// the leak path is structurally gone. Non-copyable: the group handle is
// process-wide MPP state (same lesson as RknnEngine, P0-3). A template:
// kBufCount is a NetEncoder class constant, invisible to a plain
// file-scope struct.
// 13m (2026-11-08): extracted verbatim from net_encoder.hpp -- a
// template stays header-only by construction (pod_list.hpp precedent);
// net_encoder.h keeps declarations only.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>

template <int N>
struct RkmppDrmPool
{
    MppBufferGroup group = nullptr;
    std::array<MppBuffer, N> buf {};

    RkmppDrmPool() = default;
    ~RkmppDrmPool()
    {
        release_all();
    }
    RkmppDrmPool( const RkmppDrmPool& ) = delete;
    RkmppDrmPool& operator=( const RkmppDrmPool& ) = delete;

    // Allocate all N buffers of `bytes` each and gray-fill them: the
    // 16-aligned tail columns must not show stale frames at session
    // start. Self-cleaning (P1-5 pattern): on any failure everything
    // created so far is released and the pool is left empty, so the
    // owner's throw is safe and a later retry starts clean.
    // Non-throwing: reports through `why`, the owner throws loudly.
    bool alloc( size_t bytes, const char* tag, const char* caller, std::string* why )
    {
        if ( group )
        {
            if ( why )
                *why = "drm pool: already allocated";
            return false;
        }
        const MPP_RET rg = mpp_buffer_group_get(
                &group, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, tag, caller );
        if ( rg != MPP_OK )
        {
            group = nullptr;
            if ( why )
                *why = "mpp_buffer_group_get failed";
            return false;
        }
        for ( auto& b : buf )
        {
            if ( mpp_buffer_get( group, &b, bytes ) != MPP_OK )
            {
                release_all();
                if ( why )
                    *why = "mpp_buffer_get failed";
                return false;
            }
            // Neutral gray fill: the 16-aligned tail columns of the
            // stream keep their fill (never receive RGA data).
            if ( void* p = mpp_buffer_get_ptr( b ) )
                std::memset( p, 128, bytes );
        }
        return true;
    }

private:
    void release_all()
    {
        for ( auto& b : buf )
        {
            if ( b )
            {
                mpp_buffer_put( b );
                b = nullptr;
            }
        }
        if ( group )
        {
            mpp_buffer_group_put( group );
            group = nullptr;
        }
    }
};

