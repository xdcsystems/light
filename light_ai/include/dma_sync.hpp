// DMA coherence helpers shared by pipeline (NPU tensor) and later
// by net_encoder (its Transport guard is the same RAII pattern).
// Header-only: guards are trivial RAII, cache ops are per-line one-liners.

#pragma once
#include <cstddef>  // size_t
#include <cstdint>  // uintptr_t
#include <fcntl.h>  // dma-buf sync ioctl
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

namespace dma
{

// CPU access bracket for the NPU input tensor dma-buf (--cpu-draw fix).
// draw_boxes_cpu writes through the rknn mmap with plain stores; 
// on a cacheable mapping the dirty lines stay in the CPU cache while RGA
// (encoder prep_frame crop) reads DRAM as a DMA master, so CPU-drawn
// boxes/trails reach the h264 stream stale: vertical edges smear into
// ~21-px horizontal dashes (64-byte line / 3 bytes per RGB888 pixel),
// ghost trails of previous frames bleed through. The NUT preview is
// always clean: the CPU reads back its own hot cache lines. The GPU
// path is unaffected: Mali writes are DMA-side, ordered by the native
// fence. DMA_BUF_IOCTL_SYNC START/END is the kernel contract for CPU
// access to a dma-buf; RAII guard per the no-new rule (same pattern
// as the Transport guard in net_encoder.cpp).
struct DmaSync
{
    DmaSync( int fd, bool write_access ) : fd_( fd )
    {
        if ( fd_ < 0 )
            return;
        dma_buf_sync s {};
        s.flags = write_access ? ( DMA_BUF_SYNC_RW | DMA_BUF_SYNC_START )
                               : ( DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START );
        ::ioctl( fd_, DMA_BUF_IOCTL_SYNC, &s );  // best effort
    }
    ~DmaSync()
    {
        if ( fd_ < 0 )
            return;
        dma_buf_sync s {};
        s.flags = DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END;
        ::ioctl( fd_, DMA_BUF_IOCTL_SYNC, &s );
    }
    DmaSync( const DmaSync& ) = delete;
    DmaSync& operator=( const DmaSync& ) = delete;

    int fd_;
};
static_assert( sizeof( dma_buf_sync ) == 8, "dma_buf_sync layout drift" );
// NOLINTNEXTLINE(misc-redundant-expression): intentional drift guard - the macro is defined as this very expression
static_assert( DMA_BUF_SYNC_RW == ( DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE ),
        "dma-buf sync flag semantics drift" );

// Explicit cache maintenance for CPU writes into the NPU tensor.
// DMA_BUF_IOCTL_SYNC alone proved insufficient here: the RKNPU DRM
// driver's begin/end_cpu_access on this platform does not clean the
// CPU cache for rknn_create_mem buffers, so the only reliable way to
// make CPU stores visible to non-coherent DMA readers (RGA crop,
// NPU, next-frame RGA rewrite) is to clean the lines ourselves.
// AArch64 permits userspace DC CVAC (clean by VA to the point of coherency). 
// 64 is the AArch64 cache-line size (CLIDR_EL1 = 64 on every RK3588 core);
// no named constant per the project magic-number policy (see net_mjpeg_source.cpp).

inline void cache_clean( const void* p, size_t len )
{
#if defined( __aarch64__ )
    uintptr_t addr = reinterpret_cast<uintptr_t>( p ) & ~uintptr_t( 63 );
    const uintptr_t end = reinterpret_cast<uintptr_t>( p ) + len;
    for ( ; addr < end; addr += 64 )
        __asm__ volatile( "dc cvac, %0" :: "r"( addr ) : "memory" );  // NOLINT(hicpp-no-assembler): DMA cache maintenance, no portable substitute
    __asm__ volatile( "dsb sy" ::: "memory" );  // NOLINT(hicpp-no-assembler): barrier required after DC ops
#else
    // No cache-maintenance instruction on this arch; the builtin is
    // a no-op there - the cross-check build compiles and stays honest.
    __builtin___clear_cache( const_cast<char*>( static_cast<const char*>( p ) ),
            const_cast<char*>( static_cast<const char*>( p ) ) + len );
#endif
}

// Cache DROP (clean+invalidate) for the NPU input tensor, executed
// right before the RGA DMA rewrite of the NEXT frame (patch 002).
// cache_clean() (CVAC) fixes the write side, but a cleaned line
// REMAINS VALID in the CPU cache: RGA's next-frame write updates
// DRAM only, and a draw_boxes_cpu store that hits the resident
// stale line merges the new box pixel with last frame's background
// - one-frame ghost dashes along vertical box edges (observed live).
// Dropping the lines here forces the next CPU store to miss and
// load fresh DRAM. DC CIVAC is clean+invalidate; Linux sets
// SCTLR_EL1.UCI=1, so it is permitted from userspace.
inline void cache_drop( const void* p, size_t len )
{
#if defined( __aarch64__ )
    uintptr_t addr = reinterpret_cast<uintptr_t>( p ) & ~uintptr_t( 63 );
    const uintptr_t end = reinterpret_cast<uintptr_t>( p ) + len;
    for ( ; addr < end; addr += 64 )
        __asm__ volatile( "dc civac, %0" :: "r"( addr ) : "memory" );  // NOLINT(hicpp-no-assembler): DMA cache maintenance, no portable substitute
    __asm__ volatile( "dsb sy" ::: "memory" );  // NOLINT(hicpp-no-assembler): barrier required after DC ops
#else
    // x86 is DMA-coherent; nothing to drop.
    (void) p;
    (void) len;
#endif
}

}  // namespace dma

