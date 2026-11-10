#pragma once
#include <array>

// Fixed-capacity ring (FIFO) on std::array: no heap by construction.
// Generalization of the three hand-written rings (PubFifo in slot_ring.hpp,
// Tracker::Traj in tracker.hpp, the packet queue in net_encoder.h): one
// modular-arithmetic implementation instead of three copies of
// (head + n) % cap. push() drops on overflow and returns false (same
// policy as PodList::push); front/pop carry no guards -- the owning layer
// keeps its own loud-log checks (PubFifo) or its own overflow policy
// (NetEncoder: full-queue flush + forced IDR).
// T is POD/pointer-like by convention: plain assignment, no ctors needed.
template <typename T, int N>
struct PodRing
{
    enum { cap = N };

    void clear()
    {
        head = 0;
        n = 0;
    }
    bool full() const { return n >= N; }
    bool empty() const { return n == 0; }
    int size() const { return n; }

    bool push( const T& v )
    {
        if ( n >= N )
            return false;
        d[( head + n ) % N] = v;
        ++n;
        return true;
    }
    // Claim the tail slot for an in-place fill (av_packet_move_ref
    // pattern): the caller guarantees !full(), the slot is committed
    // (++n) on return.
    T& push_slot()
    {
        T& slot = d[( head + n ) % N];
        ++n;
        return slot;
    }
    T& front() { return d[head]; }
    const T& front() const { return d[head]; }
    void pop()
    {
        head = ( head + 1 ) % N;
        --n;
    }
    // Indexing from the end: back(0) == newest, back(k) walks to oldest.
    T& back( int k )
    {
        return d[( head + n - 1 - k + N ) % N];
    }
    const T& back( int k ) const
    {
        return d[( head + n - 1 - k + N ) % N];
    }

public:
    std::array<T, N> d {};
    int head = 0;
    int n = 0;
};
