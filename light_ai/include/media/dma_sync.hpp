// DMA coherence helpers shared by the pipeline (NPU tensor), the
// encoder buffers (RkmppDrmPool gray fill) and FrameComposer / the
// NO SIGNAL plate (CPU writes into canvas dma-bufs).
// Header-only: guards are trivial RAII, cache ops are per-line one-liners.

#pragma once
#include <cerrno>   // EINTR, EAGAIN
#include <chrono>   // steady_clock deadline for bounded EAGAIN retry
#include <cstddef>  // size_t
#include <cstdint>  // uintptr_t
#include <cstring>  // std::memset
#include <time.h>   // nanosleep
#include <fcntl.h>  // dma-buf sync ioctl
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "core/non_copyable.hpp"

namespace dma
{

// CPU access bracket for the NPU input tensor dma-buf.
// draw_boxes_cpu writes through the rknn mmap with plain stores;
// on a cacheable mapping the dirty lines stay in the CPU cache
// while RGA (FrameComposer::compose crop) reads DRAM as a DMA master,
// so CPU-drawn boxes/trails reach the h264 stream stale:
// vertical edges smear into ~21-px horizontal dashes
// (64-byte cache line / 3 bytes per RGB888 pixel),
// ghost trails of previous frames bleed through.
// The NUT preview is always clean:
// the CPU reads back its own hot cache lines.
// The GPU path is unaffected: Mali writes are DMA-side, ordered by the native fence.
// DMA_BUF_IOCTL_SYNC START/END is the kernel contract for CPU access to a dma-buf;
// this RAII guard keeps the bracket unbreakable.
struct DmaSync : private NonMovable<DmaSync>
{
    DmaSync( int fd, bool write_access ) noexcept : fd_( fd ), write_( write_access )
    {
        sync( write_ ? ( DMA_BUF_SYNC_RW | DMA_BUF_SYNC_START ) : ( DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START ) );
    }
    ~DmaSync() { sync( write_ ? ( DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END ) : ( DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END ) ); }

private:
    // Best effort, but not lost to a signal or transient fence wait:
    // DMA_BUF_IOCTL_SYNC can return EINTR or EAGAIN per Linux DMA-BUF docs,
    // and an unretried START/END leaves the bracket half-open (M-15).
    void sync( unsigned long long flags ) const noexcept
    {
        if ( fd_ < 0 )
            return;
        dma_buf_sync s {};
        s.flags = flags;
        const timespec pause { 0, 100000L };  // 100 us backoff on transient EAGAIN
        std::chrono::steady_clock::time_point deadline {};
        bool has_deadline = false;
        while ( ::ioctl( fd_, DMA_BUF_IOCTL_SYNC, &s ) != 0 )
        {
            if ( errno == EINTR )
                continue;
            if ( errno != EAGAIN )
                return;
            const auto now = std::chrono::steady_clock::now();
            if ( !has_deadline )
            {
                deadline = now + std::chrono::milliseconds( 50 );
                has_deadline = true;
            }
            if ( now >= deadline )
                return;
            ::nanosleep( &pause, nullptr );
        }
    }

private:
    int fd_;
    bool write_;
};
static_assert( sizeof( dma_buf_sync ) == 8, "dma_buf_sync layout drift" );
// NOLINTNEXTLINE(misc-redundant-expression): intentional drift guard - the macro is defined as this very expression
static_assert( DMA_BUF_SYNC_RW == ( DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE ),
        "dma-buf sync flag semantics drift" );

// Explicit cache maintenance for CPU writes into the NPU tensor.
// DMA_BUF_IOCTL_SYNC alone proved insufficient here:
// the RKNPU DRM driver's begin/end_cpu_access on this platform
// does not clean the CPU cache for rknn_create_mem buffers,
// so the only reliable way to make CPU stores visible to
// non-coherent DMA readers (RGA crop, NPU, next-frame RGA rewrite)
// is to clean the lines ourselves.
// AArch64 permits userspace DC CVAC (clean by VA to the point of coherency).
// 64 is the AArch64 cache-line size (CLIDR_EL1 = 64 on every RK3588 core);
// no named constant: project magic-number policy.

inline void cache_clean( const void* p, size_t len )
{
    if ( !p || len == 0 )
        return;
#if defined( __aarch64__ )
    uintptr_t addr = reinterpret_cast<uintptr_t>( p ) & ~uintptr_t( 63 );
    const uintptr_t end = reinterpret_cast<uintptr_t>( p ) + len;
    for ( ; addr < end; addr += 64 )
        // NOLINTNEXTLINE(hicpp-no-assembler): DMA cache maintenance, no portable substitute
        __asm__ volatile( "dc cvac, %0" :: "r"( addr ) : "memory" );
    __asm__ volatile( "dsb sy" ::: "memory" );  // NOLINT(hicpp-no-assembler): barrier required after DC ops
#elif defined( __x86_64__ )
    // x86-64 is DMA-coherent; nothing to clean (p and len are used by the early return).
#else
#error "dma::cache_clean: unknown arch; add it to the coherent #elif or port the cache maintenance"
#endif
}

// Cache DROP (clean+invalidate) for the NPU input tensor before CPU reads/writes
// of DMA-written pixels (draw_boxes_cpu, CalDumper::maybe, OverlaySink preview).
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
    if ( !p || len == 0 )
        return;
#if defined( __aarch64__ )
    uintptr_t addr = reinterpret_cast<uintptr_t>( p ) & ~uintptr_t( 63 );
    const uintptr_t end = reinterpret_cast<uintptr_t>( p ) + len;
    for ( ; addr < end; addr += 64 )
        // NOLINTNEXTLINE(hicpp-no-assembler): DMA cache maintenance, no portable substitute
        __asm__ volatile( "dc civac, %0" :: "r"( addr ) : "memory" );
    __asm__ volatile( "dsb sy" ::: "memory" );  // NOLINT(hicpp-no-assembler): barrier required after DC ops
#elif defined( __x86_64__ )
    // x86-64 is DMA-coherent; nothing to drop (p and len are used by the early return).
#else
#error "dma::cache_drop: unknown arch; add it to the coherent #elif or port the cache maintenance"
#endif
}


// CPU fill of a dma-buf range, then flush the dirty lines to the
// point of coherency: the canonical "CPU writes, device reads next"
// sequence. The DmaSync bracket and the cache_clean are inseparable
// halves of one operation -- call this instead of hand-rolling either.
inline void fill_and_flush( int fd, void* p, unsigned char val, size_t size )
{
    if ( !p || size == 0 )
        return;
    const DmaSync sync( fd, true );
    std::memset( p, val, size );
    cache_clean( p, size );
}
}  // namespace dma

