#include <algorithm>
#include <cerrno>
#include <iterator>
#include <utility>
#include <sys/mman.h>

#include "core/status.hpp"
#include "source/mjpeg_slot_pool.hpp"
#include "core/throw_on.hpp"

namespace
{
    constexpr size_t kPoolBytes = static_cast<size_t>( MjpegSlotPool::kSlots ) * static_cast<size_t>( MjpegSlotPool::kSlotBytes );
}  // namespace

MjpegSlotPool::MjpegSlotPool()
{
    void* const p = ::mmap( nullptr, kPoolBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    // MAP_FAILED is ((void *) -1), not nullptr: the old ctor stored it and
    // then checked for nullptr, so a failed mmap went unnoticed (#28). Spelled
    // out: the macro's C-style cast trips the cstyle-cast check.
    if ( p == reinterpret_cast<void*>( -1 ) )
        throw_on( errno_status( Errc::NoMemory, "frame pool mmap failed", errno ), "net-src" );
    base_ = p;
    for ( int k = 0; k < kSlots; ++k )
        slot_[k] = static_cast<uint8_t*>( base_ ) + static_cast<size_t>( k ) * static_cast<size_t>( kSlotBytes );
}

MjpegSlotPool::~MjpegSlotPool()
{
    ::munmap( base_, kPoolBytes );
}

int MjpegSlotPool::free_slot( int except ) const
{
    for ( int k = 0; k < kSlots; ++k )
    {
        if ( k != except && !in_flight_[k] )
            return k;
    }
    return -1;
}

void MjpegSlotPool::release( int i )
{
    if ( i >= 0 && i < kSlots )
        in_flight_[i] = false;
}

void MjpegSlotPool::release_all()
{
    std::fill( std::begin( in_flight_ ), std::end( in_flight_ ), false );
}

void MjpegSlotPool::swap( int a, int b )
{
    std::swap( slot_[a], slot_[b] );
}
