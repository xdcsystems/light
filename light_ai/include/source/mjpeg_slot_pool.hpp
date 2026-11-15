// MjpegSlotPool: the frame slots of the net MJPEG source -- one anonymous
// mmap of kSlots x kSlotBytes, made once at construction (init-time
// allocation, the rknn_engine out_mems_ precedent: not heap, never resized),
// the closest analog of the V4L2 driver-owned buffers the source stands in
// for. Each slot index has an in-flight flag (dequeued, not yet requeued).
//
// RAII: the constructor throws std::runtime_error when the mmap fails
// ("net-src: frame pool mmap failed: <strerror>"), so a pool that exists
// always has its memory; the destructor unmaps it. Construction happens on
// the startup path only (MediaSource::use_net <- SourceChannel::configure
// <- the Pipeline ctor, inside main's try).
//
// swap( a, b ) exchanges the memory behind two slot indices (the latest-frame
// skip and the start() drain keep the freshest frame in a fixed index); the
// in-flight flags stay with the indices.
//
// POSIX only. Thread: one owner at a time (the NetMjpegSource that holds it).

#pragma once

#include <cstddef>
#include <cstdint>

class MjpegSlotPool
{
public:
    enum
    {
        kSlots = 4,
        kSlotBytes = 1 << 20,  // 1 MiB per JPEG frame cap
    };

public:
    MjpegSlotPool();
    ~MjpegSlotPool();
    MjpegSlotPool( const MjpegSlotPool& ) = delete;
    MjpegSlotPool& operator=( const MjpegSlotPool& ) = delete;
    MjpegSlotPool( MjpegSlotPool&& ) = delete;
    MjpegSlotPool& operator=( MjpegSlotPool&& ) = delete;

    // i in [0, kSlots): the caller's contract (the indices come from this pool).
    uint8_t* data( int i ) const { return slot_[i]; }
    size_t capacity() const { return static_cast<size_t>( kSlotBytes ); }

    // The first slot not in flight and != except; -1 when there is none.
    int free_slot( int except = -1 ) const;
    bool in_flight( int i ) const { return in_flight_[i]; }
    void mark( int i ) { in_flight_[i] = true; }
    // Out of range: ignored (requeue() of a foreign index).
    void release( int i );
    void release_all();
    void swap( int a, int b );

private:
    void* base_ = nullptr;
    uint8_t* slot_[kSlots] = {};
    bool in_flight_[kSlots] = {};
};
