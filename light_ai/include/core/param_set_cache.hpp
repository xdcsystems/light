// ParamSetCache: late-join support for the H.264 stream. The SPS/PPS
// prefix of the first key packet is captured once and prepended to every
// later IDR that does not carry parameter sets of its own, so a viewer
// that joins mid-stream can decode from the next IDR on (the TS muxer
// tolerates the duplicate).
//
// Byte-level Annex B over a buffer: SDK-free, host-tested
// (tests/test_param_set_cache.cpp). Thread: the mux (serve) thread only.
// No allocation per packet: the IDR buffer is reserved once (256 KiB);
// only a larger IDR grows it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/log.hpp"

// The Annex B scans, exposed for the tests.
namespace annexb
{
    // True when the first slice NAL (after any SEI/SPS/PPS/AUD) is an IDR (type 5).
    bool is_idr( const uint8_t* d, size_t n );

    // True when the packet carries SPS or PPS NAL units before its first slice.
    bool has_param_sets( const uint8_t* d, size_t n );

    // Length of the parameter-set prefix: everything before the first
    // slice / end-of-sequence NAL, if it holds an SPS; 0 = no prefix.
    size_t header_len( const uint8_t* d, size_t n );
}  // namespace annexb

struct ByteSpan
{
    uint8_t* data = nullptr;
    size_t size = 0;
};

class ParamSetCache
{
public:
    // log: the process diagnostics, owned by main(); outlives the cache.
    explicit ParamSetCache( const Log& log );

    // One packet about to be muxed. An IDR before the capture: the prefix
    // is captured, the packet goes as is. A later IDR without SPS/PPS: the
    // returned span is prefix + packet in the cache's own buffer (valid
    // until the next call). Anything else: the input span, unchanged.
    ByteSpan prepare( uint8_t* data, size_t size );

    size_t header_size() const { return hdr_.size(); }

private:
    const Log& log_;
    std::vector<uint8_t> hdr_;
    std::vector<uint8_t> idr_buf_;
    bool repeat_logged_ = false;
};
