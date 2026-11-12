#pragma once

#include <cstddef>
#include <cstdint>

// Structural JPEG probe shared by the net MJPEG source (frame validation)
// and future consumers (per-frame geometry checks, reconnect diagnostics).
// Walks markers from SOI (FFD8) up to SOS (FFDA):
//   - fill bytes (0xFF) and standalone markers are skipped;
//   - a length-delimited segment running past the buffer is an error;
//   - the FIRST SOF segment (C0..CF, except C4/C8/CC) provides the frame
//     geometry and chroma sampling (max vertical factor 2 with min 1 = 4:2:0);
//   - a second SOF, or EOI before SOS, is a corrupt header;
//   - true only when SOS is reached after a parsed SOF
//     (the old jpeg_frame_complete semantics: SOF without a scan is not a frame).

struct JpegSofInfo
{
    int w = 0;
    int h = 0;
    bool yuv420 = false;
};

namespace detail
{

inline bool is_standalone_marker( uint8_t m )
{
    return m == 0x00 || ( m >= 0xD0 && m <= 0xD8 );
}

inline bool is_sof_marker( uint8_t m )
{
    return m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
}

// seg points at the 2-byte length field; SOFn payload: precision, height,
// width, ncomp, then per component id/hv/tq (hv: V is the LOW nibble).
inline bool parse_sof_segment( const uint8_t* seg, size_t seglen, JpegSofInfo& out )
{
    if ( seglen < 8 )
        return false;
    const int fh = ( static_cast<int>( seg[3] ) << 8 ) | seg[4];
    const int fw = ( static_cast<int>( seg[5] ) << 8 ) | seg[6];
    if ( fw < 16 || fh < 16 || fw > 8192 || fh > 8192 )
        return false;
    out.w = fw;
    out.h = fh;
    const uint8_t ncomp = seg[7];
    // Strict segment validation before touching component data: per the
    // JPEG spec SOF seglen is always 8 + 3*ncomp. A shorter length with a
    // larger ncomp used to let the loop stop after the first component,
    // returning true with v_max == v_min from the Y record alone -- a
    // 4:2:0 stream then misdetects as 4:2:2 (green-bottom artifacts).
    // ncomp outside 1..4 is not a real SOF either -- reject it too.
    if ( ncomp == 0 || ncomp > 4 )
        return false;
    if ( seglen < 8 + static_cast<size_t>( ncomp ) * 3 )
        return false;
    int v_max = 1;
    int v_min = 4;
    for ( int c = 0; c < ncomp && 10 + static_cast<size_t>( c ) * 3 < seglen; ++c )
    {
        const int v = seg[9 + static_cast<size_t>( c ) * 3] & 0x0F;
        if ( v > v_max )
            v_max = v;
        if ( v > 0 && v < v_min )
            v_min = v;
    }
    out.yuv420 = ( v_max >= 2 && v_min == 1 );
    return true;
}

} // namespace detail

inline bool probe_jpeg_sof( const uint8_t* data, size_t len, JpegSofInfo& out )
{
    if ( len < 4 || data[0] != 0xFF || data[1] != 0xD8 )
        return false;
    bool seen_sof = false;
    size_t pos = 2;
    while ( pos + 2 <= len )
    {
        if ( data[pos] != 0xFF )
            return false;  // garbage before SOS: not structurally sound
        while ( pos < len && data[pos] == 0xFF )
            ++pos;  // fill bytes
        if ( pos >= len )
            return false;
        const uint8_t m = data[pos++];
        if ( m == 0xDA )
            return seen_sof;  // SOS: the structural walk ends here
        if ( m == 0xD9 )
            return false;  // EOI before SOS: truncated header
        if ( detail::is_standalone_marker( m ) )
            continue;
        if ( pos + 2 > len )
            return false;
        const size_t seglen = ( static_cast<size_t>( data[pos] ) << 8 ) | data[pos + 1];
        if ( seglen < 2 || pos + seglen > len )
            return false;
        if ( detail::is_sof_marker( m ) )
        {
            if ( seen_sof )
                return false;  // two SOFs: corrupt header
            if ( !detail::parse_sof_segment( data + pos, seglen, out ) )
                return false;
            seen_sof = true;
        }
        pos += seglen;
    }
    return false;  // ran out of data before SOS
}
