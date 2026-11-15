// IEEE 754 binary16 (half) -> binary32 (float): the element type of the
// yolov5 output heads (fp16 NCHW, see core/yolo_decoder.hpp).
//
// Every half is exactly representable as a float, so each bit pattern has
// exactly one correct result -- the two paths below must agree bit for bit
// (tests/test_fp16.cpp checks all 65536 patterns on the board):
//   - aarch64: the hardware type __fp16, a single fcvt (the production path);
//   - elsewhere (an x86 host build of the tests): fp16_to_float_soft.
// NaN: fcvt quiets a signalling NaN (FPCR.DN = 0, the Linux default: the
// payload is kept, the quiet bit set); the software path does the same.

#pragma once

#include <cstdint>
#include <cstring>

// Exact software conversion, available on every target: the reference the
// hardware path is tested against.
inline float fp16_to_float_soft( uint16_t h )
{
    const uint32_t sign = static_cast<uint32_t>( h & 0x8000U ) << 16U;
    const uint32_t exp = ( h >> 10U ) & 0x1FU;
    uint32_t mant = h & 0x03FFU;
    uint32_t bits = sign;  // +-0
    if ( exp == 31 )
    {
        // inf (mant == 0) or NaN: the payload shifts up, a NaN comes out quiet
        bits = sign | 0x7F800000U | ( mant != 0 ? 0x00400000U : 0U ) | ( mant << 13U );
    }
    else if ( exp != 0 )
    {
        bits = sign | ( ( exp + 112U ) << 23U ) | ( mant << 13U );  // rebias 15 -> 127
    }
    else if ( mant != 0 )
    {
        // subnormal half (mant * 2^-24): a normal float -- shift the leading
        // one up to the implicit bit, one exponent step per shift
        uint32_t e = 113U;  // float exponent of 2^-14, the half subnormal scale
        while ( ( mant & 0x0400U ) == 0 )
        {
            mant <<= 1U;
            --e;
        }
        bits = sign | ( e << 23U ) | ( ( mant & 0x03FFU ) << 13U );
    }
    float out = 0.0f;
    std::memcpy( &out, &bits, sizeof( out ) );
    return out;
}

inline float fp16_to_float( uint16_t h )
{
#if defined( __aarch64__ )
    __fp16 f16 = 0;
    std::memcpy( &f16, &h, sizeof( f16 ) );
    return static_cast<float>( f16 );
#else
    return fp16_to_float_soft( h );
#endif
}
