#include <algorithm>
#include <cstdint>
#include <vector>

#include "check.hpp"
#include "core/jpeg_probe.hpp"
#include "suites.hpp"

namespace
{
    using Bytes = std::vector<uint8_t>;

    void put_soi( Bytes& b )
    {
        b.insert( b.end(), { 0xFF, 0xD8 } );
    }

    // APP0 with a 2-byte payload: a length-delimited segment the probe must skip.
    void put_app0( Bytes& b )
    {
        b.insert( b.end(), { 0xFF, 0xE0, 0x00, 0x04, 'J', 'F' } );
    }

    // SOF0, 3 components; y_hv is the Y sampling byte (H high nibble, V low nibble).
    void put_sof( Bytes& b, int w, int h, uint8_t y_hv )
    {
        b.insert( b.end(), { 0xFF, 0xC0, 0x00, 0x11, 0x08 } );
        b.push_back( static_cast<uint8_t>( h >> 8 ) );
        b.push_back( static_cast<uint8_t>( h & 0xFF ) );
        b.push_back( static_cast<uint8_t>( w >> 8 ) );
        b.push_back( static_cast<uint8_t>( w & 0xFF ) );
        b.insert( b.end(), { 0x03, 0x01, y_hv, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01 } );
    }

    void put_sos( Bytes& b )
    {
        b.insert( b.end(), { 0xFF, 0xDA, 0x00, 0x02 } );
    }

    bool probe( const Bytes& b, JpegSofInfo& info )
    {
        return probe_jpeg_sof( b.data(), b.size(), info );
    }

    void check_exif_orient( TestRun& t )
    {
        Bytes ex_le;
        put_soi( ex_le );
        ex_le.insert( ex_le.end(), { 0xFF, 0xE1, 0x00, 0x1E, 'E', 'x', 'i', 'f', 0x00, 0x00, 'I', 'I', 0x2A, 0x00,
                                     0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00,
                                     0x00, 0x00, 0x08, 0x00, 0x00, 0x00 } );
        put_sof( ex_le, 720, 1280, 0x22 );
        put_sos( ex_le );
        JpegSofInfo info;
        CHECK( t, probe( ex_le, info ) && info.exif_rot_deg == 270 );

        Bytes ex_be;
        put_soi( ex_be );
        ex_be.insert( ex_be.end(), { 0xFF, 0xE1, 0x00, 0x1E, 'E', 'x', 'i', 'f', 0x00, 0x00, 'M', 'M', 0x00, 0x2A,
                                     0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00,
                                     0x00, 0x01, 0x00, 0x06, 0x00, 0x00 } );
        put_sof( ex_be, 720, 1280, 0x22 );
        put_sos( ex_be );
        JpegSofInfo info_be;
        CHECK( t, probe( ex_be, info_be ) && info_be.exif_rot_deg == 90 );

        // An IFD offset far outside the segment (0xFFFFFFFF) is ignored: the frame still parses.
        Bytes bad_ifd = ex_le;
        // SOI(2) + marker(2) + length(2) + "Exif\0\0"(6) + byte order and magic(4)
        std::fill_n( bad_ifd.begin() + 16, 4, static_cast<uint8_t>( 0xFF ) );
        JpegSofInfo info_bad;
        CHECK( t, probe( bad_ifd, info_bad ) && info_bad.exif_rot_deg == -1 && info_bad.w == 720 );
    }

    void check_null_data( TestRun& t )
    {
        JpegSofInfo info;
        CHECK( t, !probe_jpeg_sof( nullptr, 64, info ) );
        uint32_t n = 64;
        CHECK( t, !jpeg_frame_check( nullptr, n, info ) );
        CHECK_EQ( t, n, static_cast<uint32_t>( 64 ) );
    }
}  // namespace

void test_jpeg_probe( TestRun& t )
{
    // Valid 4:2:0 header: geometry and sampling come from the SOF.
    {
        Bytes b;
        put_soi( b );
        put_app0( b );
        put_sof( b, 1280, 720, 0x22 );
        put_sos( b );
        JpegSofInfo info;
        CHECK( t, probe( b, info ) );
        CHECK( t, info.w == 1280 && info.h == 720 && info.yuv420 );
    }
    // Valid 4:2:2 header (Y sampling 2x1): not 4:2:0.
    {
        Bytes b;
        put_soi( b );
        put_sof( b, 1280, 960, 0x21 );
        put_sos( b );
        JpegSofInfo info;
        CHECK( t, probe( b, info ) );
        CHECK( t, info.w == 1280 && info.h == 960 && !info.yuv420 );
    }
    // Fill bytes (0xFF runs) before a marker are skipped.
    {
        Bytes b;
        put_soi( b );
        b.insert( b.end(), { 0xFF, 0xFF } );
        put_sof( b, 640, 480, 0x22 );
        put_sos( b );
        JpegSofInfo info;
        CHECK( t, probe( b, info ) && info.w == 640 && info.h == 480 );
    }
    // Rejections: each case breaks exactly one structural rule.
    {
        JpegSofInfo info;
        const Bytes no_soi { 0x00, 0x00, 0xFF, 0xDA };
        CHECK( t, !probe( no_soi, info ) );

        Bytes no_sos;
        put_soi( no_sos );
        put_sof( no_sos, 1280, 720, 0x22 );
        CHECK( t, !probe( no_sos, info ) );  // SOF without a scan is not a frame

        Bytes no_sof;
        put_soi( no_sof );
        put_app0( no_sof );
        put_sos( no_sof );
        CHECK( t, !probe( no_sof, info ) );

        Bytes two_sof;
        put_soi( two_sof );
        put_sof( two_sof, 1280, 720, 0x22 );
        put_sof( two_sof, 1280, 720, 0x22 );
        put_sos( two_sof );
        CHECK( t, !probe( two_sof, info ) );

        Bytes eoi_first;
        put_soi( eoi_first );
        put_sof( eoi_first, 1280, 720, 0x22 );
        eoi_first.insert( eoi_first.end(), { 0xFF, 0xD9 } );
        put_sos( eoi_first );
        CHECK( t, !probe( eoi_first, info ) );

        Bytes truncated;
        put_soi( truncated );
        put_sof( truncated, 1280, 720, 0x22 );
        truncated.resize( truncated.size() - 4 );  // the SOF segment runs past the buffer
        CHECK( t, !probe( truncated, info ) );

        Bytes garbage;
        put_soi( garbage );
        garbage.insert( garbage.end(), { 0x12, 0x34 } );  // not a marker
        put_sof( garbage, 1280, 720, 0x22 );
        put_sos( garbage );
        CHECK( t, !probe( garbage, info ) );

        Bytes tiny;
        put_soi( tiny );
        put_sof( tiny, 8, 8, 0x22 );  // below the 16 px minimum
        put_sos( tiny );
        CHECK( t, !probe( tiny, info ) );

        Bytes bad_ncomp;
        put_soi( bad_ncomp );
        put_sof( bad_ncomp, 1280, 720, 0x22 );
        bad_ncomp[2 + 2 + 2 + 5] = 0x05;  // ncomp byte: SOI(2) + marker(2) + len(2) + precision/h/w(5)
        put_sos( bad_ncomp );
        CHECK( t, !probe( bad_ncomp, info ) );

        const uint8_t short_buf[3] = { 0xFF, 0xD8, 0xFF };
        CHECK( t, !probe_jpeg_sof( short_buf, sizeof( short_buf ), info ) );
    }
    // The remaining exits of the marker walk, pinned before the stage-6e split (#39).
    {
        JpegSofInfo info;
        // Standalone markers (RST0, a stuffed 0x00, a second SOI) carry no
        // length: skipped, the walk goes on to SOS.
        Bytes standalone;
        put_soi( standalone );
        standalone.insert( standalone.end(), { 0xFF, 0xD0, 0xFF, 0x00, 0xFF, 0xD8 } );
        put_sof( standalone, 1280, 720, 0x22 );
        standalone.insert( standalone.end(), { 0xFF, 0xD7 } );
        put_sos( standalone );
        CHECK( t, probe( standalone, info ) && info.w == 1280 && info.h == 720 && info.yuv420 );

        Bytes fill_to_end;
        put_soi( fill_to_end );
        put_sof( fill_to_end, 1280, 720, 0x22 );
        fill_to_end.insert( fill_to_end.end(), { 0xFF, 0xFF, 0xFF } );  // fill bytes up to the end, no marker
        CHECK( t, !probe( fill_to_end, info ) );

        Bytes no_length;
        put_soi( no_length );
        put_sof( no_length, 1280, 720, 0x22 );
        no_length.insert( no_length.end(), { 0xFF, 0xE0 } );  // APP0 with no room for its length field
        CHECK( t, !probe( no_length, info ) );

        Bytes short_length;
        put_soi( short_length );
        short_length.insert( short_length.end(), { 0xFF, 0xE0, 0x00, 0x01 } );  // length 1 < 2 (the field itself)
        put_sof( short_length, 1280, 720, 0x22 );
        put_sos( short_length );
        CHECK( t, !probe( short_length, info ) );

        Bytes short_sof;
        put_soi( short_sof );
        put_sof( short_sof, 1280, 720, 0x22 );
        short_sof[2 + 2 + 1] = 0x0E;  // SOF length 14 < 8 + 3 * 3 (low byte: SOI(2) + marker(2) + high byte(1))
        put_sos( short_sof );
        CHECK( t, !probe( short_sof, info ) );

        Bytes sos_first;
        put_soi( sos_first );
        put_sos( sos_first );
        put_sof( sos_first, 1280, 720, 0x22 );
        CHECK( t, !probe( sos_first, info ) );  // SOS before any SOF
    }
    // jpeg_frame_check: the probe plus a trailing EOI; the trailing HTTP
    // CR/LF/space is trimmed in place.
    {
        Bytes f;
        put_soi( f );
        put_sof( f, 64, 32, 0x22 );
        put_sos( f );
        f.insert( f.end(), { 0x12, 0x34, 0xFF, 0xD9 } );
        const auto clean = static_cast<uint32_t>( f.size() );
        f.insert( f.end(), { '\r', '\n', ' ' } );
        uint32_t n = static_cast<uint32_t>( f.size() );
        JpegSofInfo info;
        CHECK( t, jpeg_frame_check( f.data(), n, info ) );
        CHECK( t, n == clean && info.w == 64 && info.h == 32 );

        Bytes no_eoi( f.begin(), f.begin() + clean - 2 );
        n = static_cast<uint32_t>( no_eoi.size() );
        CHECK( t, !jpeg_frame_check( no_eoi.data(), n, info ) );

        Bytes no_sof;
        put_soi( no_sof );
        put_sos( no_sof );
        no_sof.insert( no_sof.end(), { 0x12, 0xFF, 0xD9 } );
        n = static_cast<uint32_t>( no_sof.size() );
        CHECK( t, !jpeg_frame_check( no_sof.data(), n, info ) );

        const uint8_t not_jpeg[] = { 'H', 'T', 'T', 'P', 0xFF, 0xD9 };
        n = sizeof( not_jpeg );
        CHECK( t, !jpeg_frame_check( not_jpeg, n, info ) );
    }
    check_exif_orient( t );
    check_null_data( t );
}
