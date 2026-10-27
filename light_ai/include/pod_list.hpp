#pragma once
#include <array>

// Fixed-capacity list on std::array: no heap by construction (project
// no-heap rule). push() drops on overflow and returns false — the
// pipeline caps detections by conf, so the cap is a safety net, not
// a design limit. One template, two instantiations (DetList, GpuBoxList).
template <typename T, int N>
struct PodList
{
    std::array<T, N> d {};
    int n = 0;

    void clear() { n = 0; }
    bool push( const T& v )
    {
        if ( n >= N )
            return false;
        d[n++] = v;
        return true;
    }
};
