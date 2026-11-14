#include <cstddef>
#include <cstdint>

#include "check.hpp"
#include "source/mjpeg_slot_pool.hpp"
#include "suites.hpp"

// The mmap failure itself (the constructor throws) cannot be forced on the
// host without poisoning the test process: covered by review only.
void test_mjpeg_slot_pool( TestRun& t )
{
    MjpegSlotPool p;
    const size_t cap = p.capacity();
    CHECK( t, cap == static_cast<size_t>( MjpegSlotPool::kSlotBytes ) );
    // Distinct, non-overlapping, writable end to end.
    bool apart = true;
    for ( int i = 0; i < MjpegSlotPool::kSlots; ++i )
    {
        CHECK( t, p.data( i ) != nullptr && !p.in_flight( i ) );
        p.data( i )[0] = static_cast<uint8_t>( i );
        p.data( i )[cap - 1] = static_cast<uint8_t>( 0xA0 + i );
        for ( int j = 0; j < i; ++j )
        {
            const uint8_t* a = p.data( i );
            const uint8_t* b = p.data( j );
            apart = apart && ( a >= b + cap || b >= a + cap );
        }
    }
    CHECK( t, apart );
    for ( int i = 0; i < MjpegSlotPool::kSlots; ++i )
        CHECK( t, p.data( i )[0] == i && p.data( i )[cap - 1] == 0xA0 + i );
    // free_slot: the first not in flight, skipping except; -1 when none.
    CHECK( t, p.free_slot() == 0 && p.free_slot( 0 ) == 1 );
    p.mark( 0 );
    p.mark( 2 );
    CHECK( t, p.in_flight( 0 ) && p.in_flight( 2 ) && p.free_slot() == 1 && p.free_slot( 1 ) == 3 );
    p.mark( 1 );
    p.mark( 3 );
    CHECK( t, p.free_slot() == -1 );
    p.release( 2 );
    p.release( -1 );  // a foreign index: ignored
    p.release( MjpegSlotPool::kSlots );
    CHECK( t, p.free_slot() == 2 && p.free_slot( 2 ) == -1 );
    p.release_all();
    CHECK( t, p.free_slot() == 0 && !p.in_flight( 3 ) );
    // swap: the memory moves, the in-flight flags stay with the indices.
    uint8_t* const m0 = p.data( 0 );
    uint8_t* const m1 = p.data( 1 );
    p.mark( 0 );
    p.swap( 0, 1 );
    CHECK( t, p.data( 0 ) == m1 && p.data( 1 ) == m0 && p.in_flight( 0 ) && !p.in_flight( 1 ) );
    CHECK( t, p.data( 0 )[0] == 1 && p.data( 1 )[0] == 0 );
}
