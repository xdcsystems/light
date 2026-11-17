#include <cstdint>
#include <cstring>

#include "core/fp16.hpp"
#include "check.hpp"
#include "suites.hpp"

namespace
{
    uint32_t float_bits( float f )
    {
        uint32_t b = 0;
        std::memcpy( &b, &f, sizeof( b ) );
        return b;
    }
}  // namespace

void test_fp16( TestRun& t )
{
    // The software path against known values (both targets).
    CHECK( t, float_bits( fp16_to_float_soft( 0x0000 ) ) == 0x00000000U );  // +0
    CHECK( t, float_bits( fp16_to_float_soft( 0x8000 ) ) == 0x80000000U );  // -0
    CHECK( t, fp16_to_float_soft( 0x3C00 ) == 1.0f );
    CHECK( t, fp16_to_float_soft( 0xC000 ) == -2.0f );
    CHECK( t, fp16_to_float_soft( 0x3555 ) == 0.333251953125f );
    CHECK( t, fp16_to_float_soft( 0x7BFF ) == 65504.0f );                   // largest normal
    CHECK( t, fp16_to_float_soft( 0x0400 ) == 6.103515625e-05f );           // smallest normal, 2^-14
    CHECK( t, fp16_to_float_soft( 0x0001 ) == 5.9604644775390625e-08f );    // smallest subnormal, 2^-24
    CHECK( t, fp16_to_float_soft( 0x03FF ) == 6.0975551605224609375e-05f ); // largest subnormal, 1023 * 2^-24
    CHECK( t, fp16_to_float_soft( 0x8200 ) == -3.0517578125e-05f );         // -512 * 2^-24
    CHECK( t, float_bits( fp16_to_float_soft( 0x7C00 ) ) == 0x7F800000U );  // +inf
    CHECK( t, float_bits( fp16_to_float_soft( 0xFC00 ) ) == 0xFF800000U );  // -inf
    CHECK( t, float_bits( fp16_to_float_soft( 0x7E00 ) ) == 0x7FC00000U );  // quiet NaN
    CHECK( t, float_bits( fp16_to_float_soft( 0x7D01 ) ) == 0x7FE02000U );  // signalling NaN: quieted, payload kept
    CHECK( t, float_bits( fp16_to_float_soft( 0xFE01 ) ) == 0xFFC02000U );  // negative NaN

    // Every bit pattern: the production path equals the software one bit for
    // bit. On aarch64 that is the hardware fcvt against the software path; on
    // another host both sides are the software path (the known values above
    // carry the check there).
    int mismatches = 0;
    for ( uint32_t h = 0; h <= 0xFFFFU; ++h )
    {
        const auto v = static_cast<uint16_t>( h );
        if ( float_bits( fp16_to_float( v ) ) != float_bits( fp16_to_float_soft( v ) ) )
            ++mismatches;
    }
    CHECK( t, mismatches == 0 );
}
