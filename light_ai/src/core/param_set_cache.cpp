#include <cstring>

#include "core/param_set_cache.hpp"

namespace
{
    // Start-code detector shared by header_len() and has_param_sets():
    // a code begins at its FIRST zero, both 3-byte (00 00 01) and
    // 4-byte (00 00 00 01) prefixes. Inside a NAL payload 00 00 01
    // cannot occur (emulation prevention), and 00 00 00 continues with
    // 01 only as a longer code, so there are no false positives. A
    // one-byte-at-a-time scan lands on the true NAL boundary; a 3-byte-only
    // scan stops on the SECOND zero of a 4-byte code and lets a stray 00
    // leak into the header tail (00 00 00 00 01 glued before the IDR
    // start code downstream).
    bool is_code_at( const uint8_t* d, size_t p, size_t n )
    {
        return p + 3 <= n && d[p] == 0 && d[p + 1] == 0
               && ( d[p + 2] == 1 || ( p + 4 <= n && d[p + 2] == 0 && d[p + 3] == 1 ) );
    }

    size_t next_code( const uint8_t* d, size_t i, size_t n )
    {
        while ( i < n && !is_code_at( d, i, n ) )
            ++i;
        return i;
    }

    size_t code_len_at( const uint8_t* d, size_t i )
    {
        return ( d[i + 2] == 1 ) ? 3 : 4;
    }

    bool is_slice_or_eos( uint8_t t )
    {
        return t == 5 || t == 1 || t == 10;
    }

    enum
    {
        kIdrReserve = 256 * 1024
    };
}  // namespace

namespace annexb
{
    bool is_idr( const uint8_t* d, size_t n )
    {
        for ( size_t i = 0; i + 4 <= n; ++i )
            if ( d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 )
            {
                const uint8_t t = d[i + 3] & 0x1F;
                if ( t == 5 )
                    return true;
                if ( t == 6 || t == 7 || t == 8 || t == 9 )
                    continue;
                return false;
            }
        return false;
    }

    bool has_param_sets( const uint8_t* d, size_t n )
    {
        size_t i = next_code( d, 0, n );
        while ( i < n && is_code_at( d, i, n ) )
        {
            const size_t cl = code_len_at( d, i );
            if ( i + cl >= n )
                return false;  // truncated NAL type byte
            const uint8_t t = d[i + cl] & 0x1F;
            if ( t == 7 || t == 8 )  // SPS or PPS already in the packet
                return true;
            if ( is_slice_or_eos( t ) )
                break;  // slice/EoS NAL reached without SPS/PPS
            i = next_code( d, i + cl, n );
        }
        return false;
    }

    size_t header_len( const uint8_t* d, size_t n )
    {
        size_t i = next_code( d, 0, n );
        if ( i >= n )
            return 0;
        bool saw_sps = false;
        while ( is_code_at( d, i, n ) )
        {
            const size_t cl = code_len_at( d, i );
            if ( i + cl >= n )
                break;  // truncated NAL type byte: nothing sane past it
            const uint8_t t = d[i + cl] & 0x1F;
            if ( t == 7 )
                saw_sps = true;
            if ( is_slice_or_eos( t ) )
                break;  // slice/EoS NAL: the header ends here
            const size_t j = next_code( d, i + cl, n );
            if ( j >= n )
            {
                i = n;  // no further start code: the header runs to the packet end
                break;
            }
            i = j;
        }
        return ( saw_sps && i > 0 ) ? i : 0;
    }
}  // namespace annexb

ParamSetCache::ParamSetCache( const Log& log ) : log_( log )
{
    idr_buf_.reserve( kIdrReserve );
}

ByteSpan ParamSetCache::prepare( uint8_t* data, size_t size )
{
    const ByteSpan as_is { data, size };
    if ( !annexb::is_idr( data, size ) )
        return as_is;
    if ( hdr_.empty() )
    {
        const size_t hl = annexb::header_len( data, size );
        if ( hl > 0 )
        {
            hdr_.assign( data, data + hl );
            log_.info() << "enc: h264 header captured: " << hdr_.size() << " bytes, repeating on every IDR\n";
        }
        return as_is;
    }
    if ( annexb::has_param_sets( data, size ) )
        return as_is;
    const size_t total = hdr_.size() + size;
    idr_buf_.resize( total );
    std::memcpy( idr_buf_.data(), hdr_.data(), hdr_.size() );
    std::memcpy( idr_buf_.data() + hdr_.size(), data, size );
    if ( !repeat_logged_ )
    {
        log_.info() << "enc: header repeat active\n";
        repeat_logged_ = true;
    }
    return ByteSpan { idr_buf_.data(), total };
}
