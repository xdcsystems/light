#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "core/param_set_cache.hpp"
#include "suites.hpp"

namespace
{
    using Bytes = std::vector<uint8_t>;

    // One NAL unit with a 4-byte (long) or 3-byte start code; the payload
    // carries no zero pairs (no emulation prevention needed).
    Bytes nal( uint8_t type, bool long_code, int payload = 3 )
    {
        Bytes b;
        if ( long_code )
            b.push_back( 0 );
        b.insert( b.end(), { 0, 0, 1, type } );
        for ( int i = 0; i < payload; ++i )
            b.push_back( static_cast<uint8_t>( 0x80 + i ) );
        return b;
    }

    Bytes cat( std::initializer_list<Bytes> parts )
    {
        Bytes out;
        for ( const Bytes& p : parts )
            out.insert( out.end(), p.begin(), p.end() );
        return out;
    }

    const uint8_t kSps = 0x67;  // nal_ref_idc 3, type 7
    const uint8_t kPps = 0x68;  // type 8
    const uint8_t kIdr = 0x65;  // type 5
    const uint8_t kP = 0x41;    // type 1
    const uint8_t kSei = 0x06;  // type 6
    const uint8_t kAud = 0x09;  // type 9

    size_t count( const std::string& s, const char* what )
    {
        size_t n = 0;
        for ( size_t p = s.find( what ); p != std::string::npos; p = s.find( what, p + 1 ) )
            ++n;
        return n;
    }

    void scans( TestRun& t )
    {
        const Bytes key = cat( { nal( kSps, true ), nal( kPps, true ), nal( kIdr, true, 8 ) } );
        const Bytes sei_idr = cat( { nal( kSei, false ), nal( kIdr, false, 8 ) } );
        const Bytes p = nal( kP, true, 8 );
        const Bytes aud_p = cat( { nal( kAud, false ), nal( kP, false ) } );

        CHECK( t, annexb::is_idr( key.data(), key.size() ) );
        CHECK( t, annexb::is_idr( sei_idr.data(), sei_idr.size() ) );
        CHECK( t, !annexb::is_idr( p.data(), p.size() ) );
        CHECK( t, !annexb::is_idr( aud_p.data(), aud_p.size() ) );
        CHECK( t, !annexb::is_idr( key.data(), 0 ) );

        CHECK( t, annexb::has_param_sets( key.data(), key.size() ) );
        CHECK( t, !annexb::has_param_sets( sei_idr.data(), sei_idr.size() ) );
        CHECK( t, !annexb::has_param_sets( p.data(), p.size() ) );
        const Bytes cut { 0, 0, 1 };  // a start code with no NAL type byte
        CHECK( t, !annexb::has_param_sets( cut.data(), cut.size() ) );

        // The prefix ends at the FIRST zero of the IDR's 4-byte start code:
        // no stray 00 leaks into the header tail.
        const size_t hdr = nal( kSps, true ).size() + nal( kPps, true ).size();
        CHECK( t, annexb::header_len( key.data(), key.size() ) == hdr );
        // Mixed code lengths: SPS 3-byte, PPS 4-byte, IDR 3-byte.
        const Bytes mixed = cat( { nal( kSps, false ), nal( kPps, true ), nal( kIdr, false ) } );
        CHECK( t, annexb::header_len( mixed.data(), mixed.size() ) == nal( kSps, false ).size() + nal( kPps, true ).size() );
        // No SPS before the slice: no prefix.
        CHECK( t, annexb::header_len( sei_idr.data(), sei_idr.size() ) == 0 );
        CHECK( t, annexb::header_len( p.data(), p.size() ) == 0 );
        // Parameter sets only: the prefix runs to the packet end.
        const Bytes only = cat( { nal( kSps, true ), nal( kPps, true ) } );
        CHECK( t, annexb::header_len( only.data(), only.size() ) == only.size() );
        // A truncated start code after the SPS ends the prefix there.
        const Bytes trunc = cat( { nal( kSps, true ), cut } );
        CHECK( t, annexb::header_len( trunc.data(), trunc.size() ) == nal( kSps, true ).size() );
        // No start code at all.
        const Bytes junk { 1, 2, 3, 4, 5 };
        CHECK( t, annexb::header_len( junk.data(), junk.size() ) == 0 );
    }

    void cache( TestRun& t )
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        ParamSetCache c( log );

        Bytes p = nal( kP, true, 8 );
        Bytes sei_idr = cat( { nal( kSei, false ), nal( kIdr, false, 8 ) } );
        Bytes key = cat( { nal( kSps, true ), nal( kPps, true ), nal( kIdr, true, 8 ) } );
        Bytes bare = nal( kIdr, true, 8 );
        const size_t hdr = nal( kSps, true ).size() + nal( kPps, true ).size();

        // Before any capture: non-key packets and an IDR without SPS pass as is.
        ByteSpan s = c.prepare( p.data(), p.size() );
        CHECK( t, s.data == p.data() && s.size == p.size() );
        s = c.prepare( sei_idr.data(), sei_idr.size() );
        CHECK( t, s.data == sei_idr.data() && c.header_size() == 0 );

        // The first key packet: captured, sent as is.
        s = c.prepare( key.data(), key.size() );
        CHECK( t, s.data == key.data() && s.size == key.size() );
        CHECK( t, c.header_size() == hdr );
        CHECK( t, count( out.str(), "enc: h264 header captured: 16 bytes, repeating on every IDR\n" ) == 1 );

        // A later bare IDR: prefix + packet, in the cache's buffer.
        s = c.prepare( bare.data(), bare.size() );
        CHECK( t, s.data != bare.data() && s.size == hdr + bare.size() );
        CHECK( t, std::memcmp( s.data, key.data(), hdr ) == 0 );
        CHECK( t, std::memcmp( s.data + hdr, bare.data(), bare.size() ) == 0 );
        CHECK( t, count( out.str(), "enc: header repeat active\n" ) == 1 );

        // Logged once only; an IDR with its own parameter sets and a P frame pass as is.
        s = c.prepare( sei_idr.data(), sei_idr.size() );
        CHECK( t, s.size == hdr + sei_idr.size() );
        CHECK( t, count( out.str(), "enc: header repeat active\n" ) == 1 );
        s = c.prepare( key.data(), key.size() );
        CHECK( t, s.data == key.data() );
        s = c.prepare( p.data(), p.size() );
        CHECK( t, s.data == p.data() );
        CHECK( t, count( out.str(), "captured" ) == 1 );
        CHECK( t, err.str().empty() );
    }
}  // namespace

void test_param_set_cache( TestRun& t )
{
    scans( t );
    cache( t );
}
