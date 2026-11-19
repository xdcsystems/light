// ParamSetCache: late-join support for the H.264 stream. The SPS/PPS
// prefix of the first key packet is captured once and prepended to every
// later IDR that does not carry parameter sets of its own, so a viewer
// that joins mid-stream can decode from the next IDR on (the TS muxer
// tolerates the duplicate).
//
// Byte-level Annex B over a buffer: SDK-free, host-tested
// (tests/test_param_set_cache.cpp). Thread: the mux (serve) thread only.
// No allocation per packet: the IDR buffer is sized once, at construction,
// for the largest packet the stream can carry (max_packet_bytes, see
// max_h264_packet()) and never grows (M-15). A packet beyond it -- unlikely:
// only emulation prevention bytes or many slices can exceed the bound --
// goes out as is, without the prefix, with one warn.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/log.hpp"

// The Annex B scans, exposed for the tests.
namespace annexb
{
    // True when the access unit's first VCL NAL is an IDR slice (type 5).
    // Every non-VCL NAL before it (SEI, SPS, PPS, AUD, filler, SPS
    // extension, prefix NAL, ...) is skipped NAL by NAL; a non-IDR slice
    // (types 1..4) first means false.
    bool is_idr( const uint8_t* d, size_t n );

    // True when the packet carries SPS or PPS NAL units before its first slice.
    bool has_param_sets( const uint8_t* d, size_t n );

    // Length of the parameter-set prefix: everything before the first
    // slice / end-of-sequence NAL, if it holds an SPS; 0 = no prefix.
    size_t header_len( const uint8_t* d, size_t n );
}  // namespace annexb

// A practical bound on one coded H.264 frame of a w x h 4:2:0 picture: the
// A.3.1 macroblock_layer limit (128 + RawMbBits = 3200 bits = 400 B per MB)
// plus 4 KiB for slice headers, AUD and SEI. It bounds the RBSP; emulation
// prevention bytes or many slices could in theory exceed it -- that case is
// covered by prepare()'s pass-through. A real CBR IDR is a few percent of it.
inline size_t max_h264_packet( int w, int h ) noexcept
{
    if ( w <= 0 || h <= 0 )
        return 0;
    const size_t mbs = static_cast<size_t>( ( w + 15 ) / 16 ) * static_cast<size_t>( ( h + 15 ) / 16 );
    return mbs * ( 384 + 16 ) + 4096;
}

struct ByteSpan
{
    uint8_t* data = nullptr;
    size_t size = 0;
};

class ParamSetCache
{
public:
    // log: the process diagnostics, owned by main(); outlives the cache.
    // max_packet_bytes: the largest packet to repeat the prefix on
    // (max_h264_packet() of the stream size); the buffer holds the prefix too.
    explicit ParamSetCache( const Log& log, size_t max_packet_bytes = kDefaultPacketCap );

    // One packet about to be muxed. An IDR before the capture: the prefix
    // is captured, the packet goes as is. A later IDR without SPS/PPS: the
    // returned span is prefix + packet in the cache's own buffer (valid
    // until the next call). Anything else: the input span, unchanged.
    ByteSpan prepare( uint8_t* data, size_t size );

    size_t header_size() const noexcept { return hdr_len_; }

private:
    enum : size_t
    {
        kHdrCap = 256,
        kDefaultPacketCap = 256 * 1024,  // the tests' default; MuxSession passes max_h264_packet()
    };

private:
    const Log& log_;
    std::array<uint8_t, kHdrCap> hdr_ {};
    size_t hdr_len_ = 0;
    std::vector<uint8_t> idr_buf_;
    bool repeat_logged_ = false;
    bool oversized_logged_ = false;
    bool too_big_logged_ = false;  // an IDR beyond idr_buf_ capacity went out without the prefix
};
