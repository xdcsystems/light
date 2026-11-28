#pragma once

#include <algorithm>
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
    int exif_rot_deg = -1;  // -1 = absent; otherwise 0, 90, 180, or 270 CW from EXIF Orientation
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
        v_max = std::max( v_max, v );
        if ( v > 0 )
            v_min = std::min( v_min, v );
    }
    out.yuv420 = ( v_max >= 2 && v_min == 1 );
    return true;
}

// pos at the 0xFF that starts a marker: skips the fill bytes (0xFF runs)
// and reads the marker byte into m, pos ends just past it. False on
// garbage where a marker must be, or on fill bytes up to the end.
inline bool read_marker( const uint8_t* data, size_t len, size_t& pos, uint8_t& m )
{
    if ( data[pos] != 0xFF )
        return false;  // garbage before SOS: not structurally sound
    while ( pos < len && data[pos] == 0xFF )
        ++pos;  // fill bytes
    if ( pos >= len )
        return false;
    m = data[pos++];
    return true;
}

// pos just past marker m, which carries a length-delimited segment:
// checks the length, parses the FIRST SOF (a second one is a corrupt
// header), pos ends past the segment. False on any structural error.
inline uint16_t exif_u16( const uint8_t* p, bool le )
{
    return le ? static_cast<uint16_t>( p[0] | ( static_cast<uint16_t>( p[1] ) << 8 ) )
              : static_cast<uint16_t>( ( static_cast<uint16_t>( p[0] ) << 8 ) | p[1] );
}

inline uint32_t exif_u32( const uint8_t* p, bool le )
{
    return le ? ( static_cast<uint32_t>( p[0] ) | ( static_cast<uint32_t>( p[1] ) << 8 ) |
                  ( static_cast<uint32_t>( p[2] ) << 16 ) | ( static_cast<uint32_t>( p[3] ) << 24 ) )
              : ( ( static_cast<uint32_t>( p[0] ) << 24 ) | ( static_cast<uint32_t>( p[1] ) << 16 ) |
                  ( static_cast<uint32_t>( p[2] ) << 8 ) | static_cast<uint32_t>( p[3] ) );
}

inline int exif_orient_to_deg( uint16_t orient )
{
    switch ( orient )
    {
        case 1: return 0;
        case 3: return 180;
        case 6: return 90;
        case 8: return 270;
        default: return -1;
    }
}

inline int parse_exif_orient( const uint8_t* seg, size_t seglen )
{
    if ( seglen < 16 || seg[2] != 'E' || seg[3] != 'x' || seg[4] != 'i' || seg[5] != 'f' || seg[6] != 0 || seg[7] != 0 )
        return -1;
    const uint8_t* tiff = seg + 8;
    const size_t tiff_len = seglen - 8;
    const bool le = tiff[0] == 'I' && tiff[1] == 'I' && tiff[2] == 0x2A && tiff[3] == 0x00;
    const bool be = tiff[0] == 'M' && tiff[1] == 'M' && tiff[2] == 0x00 && tiff[3] == 0x2A;
    if ( !le && !be )
        return -1;
    const size_t ifd_off = exif_u32( tiff + 4, le );
    // Bounds by subtraction: no offset from the file is added before it is checked (CERT INT30-C).
    if ( tiff_len < 2 || ifd_off > tiff_len - 2 )
        return -1;
    const size_t n_ent = exif_u16( tiff + ifd_off, le );
    const size_t entries_start = ifd_off + 2;
    const size_t avail = tiff_len - entries_start;
    const size_t max_ent = avail / 12;
    for ( size_t i = 0; i < n_ent && i < max_ent; ++i )
    {
        const uint8_t* e = tiff + entries_start + i * 12;
        if ( exif_u16( e, le ) == 0x0112 && exif_u16( e + 2, le ) == 3 && exif_u32( e + 4, le ) == 1 )
            return exif_orient_to_deg( exif_u16( e + 8, le ) );
    }
    return -1;
}

inline bool take_segment( const uint8_t* data, size_t len, size_t& pos, uint8_t m, bool& seen_sof, JpegSofInfo& out )
{
    if ( pos + 2 > len )
        return false;
    const size_t seglen = ( static_cast<size_t>( data[pos] ) << 8 ) | data[pos + 1];
    if ( seglen < 2 || pos + seglen > len )
        return false;
    if ( m == 0xE1 && out.exif_rot_deg < 0 )
    {
        const int rot = parse_exif_orient( data + pos, seglen );
        if ( rot >= 0 )
            out.exif_rot_deg = rot;
    }
    if ( is_sof_marker( m ) )
    {
        if ( seen_sof || !parse_sof_segment( data + pos, seglen, out ) )
            return false;  // two SOFs, or a bad SOF segment
        seen_sof = true;
    }
    pos += seglen;
    return true;
}

} // namespace detail

inline bool probe_jpeg_sof( const uint8_t* data, size_t len, JpegSofInfo& out )
{
    if ( data == nullptr )
        return false;
    if ( len < 4 || data[0] != 0xFF || data[1] != 0xD8 )
        return false;
    bool seen_sof = false;
    size_t pos = 2;
    while ( pos + 2 <= len )
    {
        uint8_t m = 0;
        if ( !detail::read_marker( data, len, pos, m ) )
            return false;
        if ( m == 0xDA )
            return seen_sof;  // SOS: the structural walk ends here
        if ( m == 0xD9 )
            return false;  // EOI before SOS: truncated header
        if ( detail::is_standalone_marker( m ) )
            continue;
        if ( !detail::take_segment( data, len, pos, m, seen_sof, out ) )
            return false;
    }
    return false;  // ran out of data before SOS
}

// One received MJPEG frame: structural validation via probe_jpeg_sof plus a
// trailing EOI. size is trimmed of trailing HTTP CR/LF/space IN PLACE (the
// callers hand the trimmed length on); any structural doubt returns false
// (the caller's corrupt-frame streak). info: the SOF geometry when true.
inline bool jpeg_frame_check( const uint8_t* data, uint32_t& size, JpegSofInfo& info )
{
    if ( data == nullptr )
        return false;
    uint32_t sz = size;
    while ( sz > 2 && ( data[sz - 1] == '\r' || data[sz - 1] == '\n' || data[sz - 1] == ' ' ) )
        --sz;
    size = sz;
    if ( sz < 4 || data[0] != 0xFF || data[1] != 0xD8 )
        return false;
    uint32_t zsz = sz;
    while ( zsz > 4 && data[zsz - 1] == 0x00 )
        --zsz;
    if ( data[zsz - 2] == 0xFF && data[zsz - 1] == 0xD9 )
    {
        sz = zsz;
        size = zsz;
    }
    if ( !probe_jpeg_sof( data, sz, info ) )
        return false;
    return data[sz - 2] == 0xFF && data[sz - 1] == 0xD9;
}
