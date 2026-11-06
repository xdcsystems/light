// Publish FIFO extracted from pipeline.hpp (stage 4): strictly
// single-threaded - dispatch, publish, drain and pool_discard all
// run on the pipeline thread, so no locks by design. Depth 3 ==
// the maximum NPU pool size. Body below is verbatim from pipeline.hpp.

#ifndef SLOT_RING_HPP
#define SLOT_RING_HPP

#include <iostream>

// Publish FIFO: strictly single-threaded — dispatch, publish, drain
// and pool_discard all run on the pipeline thread, so no locks by
// design. Depth 3 == the maximum NPU pool size.
struct PubFifo
{
    int slots[3] = {};
    int head = 0;
    int n = 0;

    void push( int s )
    {
        // Defensive guard: the depth-3 invariant ("in pub_ live exactly
        // the slots with st != FREE; at most npool_ <= 3 of them") is
        // proven by the slot machine, but it lives in comments across
        // three functions. One refactor away from a silent corruption
        // (push would overwrite slots[head] and inflate n past front/pop
        // coherence) -- so the violation gets a loud log instead.
        // Strictly single-threaded by design (see struct comment): the
        // cerr race risk with worker output exists only if this ever
        // stops being true.
        if ( n >= 3 )
        {
            std::cerr << "pubfifo: overflow (n=" << n << ", head=" << head
                      << "), incoming slot " << s << " dropped\n";
            return;
        }
        slots[( head + n ) % 3] = s;
        ++n;
    }
    int front() const
    {
        return slots[head];
    }
    void pop()
    {
        head = ( head + 1 ) % 3;
        --n;
    }
};

#endif  // SLOT_RING_HPP
