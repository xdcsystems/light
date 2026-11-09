#pragma once
#include <array>

// Fixed-capacity list on std::array: no heap by construction.
// push() drops on overflow and returns false - the pipeline caps detections by conf,
// so the cap is a safety net, not a design limit.
// One template, three instantiations (DetList, GpuBoxList, TrajList).
template <typename T, int N>
struct PodList
{
    enum { cap = N };

    void clear() { n = 0; }
    bool push( const T& v )
    {
        if ( n >= N )
            return false;
        d[n++] = v;
        return true;
    }

public:
    std::array<T, N> d {};
    int n = 0;
};
