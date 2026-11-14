// Test helpers for the MJPEG pull-stream parsers: fixture loading
// (tests/data/), a multipart stream builder, and drive() -- the
// NetMjpegSource read loop without a socket: a ByteWindow refilled in
// chunks (as much as fits, one byte at a time, or random sizes from a fixed
// seed), a desync drops the window and resyncs the reader, a full window
// without progress is a desync (net_fill's wedged-full window).

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "core/byte_window.hpp"
#include "core/multipart_reader.hpp"

#ifndef LIGHT_AI_TEST_DATA_DIR
#error "LIGHT_AI_TEST_DATA_DIR is set by tests/CMakeLists.txt"
#endif

namespace mjpeg_test
{

using Bytes = std::vector<uint8_t>;

// tests/data/<name>; empty when missing (the callers CHECK the size).
inline Bytes load_fixture( const char* name )
{
    std::ifstream f( std::string( LIGHT_AI_TEST_DATA_DIR ) + "/" + name, std::ios::binary );
    return Bytes( std::istreambuf_iterator<char>( f ), std::istreambuf_iterator<char>() );
}

inline void put( Bytes& b, const std::string& s )
{
    b.insert( b.end(), s.begin(), s.end() );
}

inline void put( Bytes& b, const Bytes& s )
{
    b.insert( b.end(), s.begin(), s.end() );
}

// One part per body: [eol] delim eol Content-Type eol [Content-Length eol] eol body;
// after the last body, when close: eol delim eol (it terminates a body without
// Content-Length). delim is the line as sent ("--frame"); eol is "\r\n" or "\n".
inline Bytes make_stream( const std::string& delim, const std::vector<Bytes>& bodies, bool with_length,
                          const std::string& eol = "\r\n", bool close = true )
{
    Bytes s;
    for ( size_t i = 0; i < bodies.size(); ++i )
    {
        if ( i > 0 )
            put( s, eol );
        put( s, delim + eol + "Content-Type: image/jpeg" + eol );
        if ( with_length )
            put( s, "Content-Length: " + std::to_string( bodies[i].size() ) + eol );
        put( s, eol );
        put( s, bodies[i] );
    }
    if ( close )
        put( s, eol + delim + eol );
    return s;
}

enum class Chunks : uint8_t
{
    Whole,    // as much as the window takes
    OneByte,  // one byte per refill
    Random    // 1..97 bytes per refill, std::mt19937 with a fixed seed
};

struct DriveResult
{
    std::vector<Bytes> parts;
    int desyncs = 0;
    size_t pending = 0;  // unconsumed bytes when the stream ran out
};

template <size_t N>
DriveResult drive( MultipartReader& r, const Bytes& s, Chunks mode, size_t cap, uint32_t seed = 7 )
{
    ByteWindow<N> w;
    Bytes dst( cap );
    std::mt19937 rng( seed );
    std::uniform_int_distribution<size_t> pick( 1, 97 );
    DriveResult out;
    size_t fed = 0;
    while ( true )
    {
        const MultipartReader::Step st = r.feed( w.data(), w.size(), dst.data() );
        w.consume( st.consumed );
        if ( st.ev == MultipartReader::Event::Part )
        {
            out.parts.emplace_back( dst.begin(), dst.begin() + st.size );
            continue;
        }
        w.tail();
        if ( st.ev == MultipartReader::Event::Desync || w.room() == 0 )
        {
            ++out.desyncs;
            w.clear();
            r.resync();
            continue;
        }
        if ( fed == s.size() )
            break;
        size_t k = w.room();
        if ( mode == Chunks::OneByte )
            k = 1;
        else if ( mode == Chunks::Random )
            k = pick( rng );
        if ( k > s.size() - fed )
            k = s.size() - fed;
        fed += w.append( s.data() + fed, k );
    }
    out.pending = w.size();
    return out;
}

}  // namespace mjpeg_test
