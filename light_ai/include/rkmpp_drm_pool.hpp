// MPP DRM pool as one RAII sub-object: the buffer group plus its N
// NV12 dma-bufs, fixed capacity on std::array (no heap). A fully
// constructed sub-object is destroyed even when the owner's
// constructor throws, so a partial allocation cannot leak the pool.
// Non-copyable: the group handle is process-wide MPP state.
// A template over the buffer count: EncoderFramePool holds the
// encoder ring (RkmppDrmPool<EncoderFramePool::kSlots>), FrameComposer
// its two canvases (RkmppDrmPool<1> each).

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

// Same include form as mpp_decoder.hpp: <rockchip/rk_mpi.h> resolves to
// the self-built /usr/local/include/rockchip headers. The quoted form
// "rk_mpi.h" hits the distro /usr/include/rockchip instead, and the two
// header sets redefine the same MppFrame enums (build failure).
#include <rockchip/rk_mpi.h>

#include "core/status.hpp"
#include "dma_sync.hpp"

template <int N>
struct RkmppDrmPool
{
    RkmppDrmPool() = default;
    ~RkmppDrmPool()
    {
        release_all();
    }
    RkmppDrmPool( const RkmppDrmPool& ) = delete;
    RkmppDrmPool& operator=( const RkmppDrmPool& ) = delete;

    // Allocate all N buffers of `bytes` each and gray-fill them:
    // the 16-aligned tail columns must not show stale frames at session start.
    // Self-cleaning on failure: everything created so far is
    // released and the pool is left empty, so the owner's throw is
    // safe and a later retry starts clean.
    // Non-throwing: returns a Status; the owner throws via throw_on (startup only).
    Status alloc( size_t bytes, const char* tag, const char* caller )
    {
        if ( group )
            return make_status( Errc::Failed, "drm pool: already allocated" );
        const MPP_RET rg = mpp_buffer_group_get(
                &group, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, tag, caller );
        if ( rg != MPP_OK )
        {
            group = nullptr;
            return sdk_status( Errc::NoMemory, "mpp_buffer_group_get failed", rg );
        }
        for ( auto& b : buf )
        {
            const MPP_RET rc = mpp_buffer_get( group, &b, bytes );
            if ( rc != MPP_OK )
            {
                release_all();
                return sdk_status( Errc::NoMemory, "mpp_buffer_get failed", rc );
            }
            // Neutral gray fill: the 16-aligned tail columns of the
            // stream keep their fill (never receive RGA data).
            if ( void* p = mpp_buffer_get_ptr( b ) )
                dma::fill_and_flush( mpp_buffer_get_fd( b ), p, 128, bytes );
        }
        return Status{};
    }

public:
    MppBufferGroup group = nullptr;
    std::array<MppBuffer, N> buf {};

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

