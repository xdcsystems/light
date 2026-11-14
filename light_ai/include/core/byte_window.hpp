// ByteWindow<N>: the receive window of a byte stream -- a fixed array with
// a read position (pos) and a write position (end). The reader looks at
// data()/size() and consume()s what it has parsed; the writer compacts the
// unconsumed bytes to the front (tail()), writes into the free room and
// commit()s it. full() = N unconsumed bytes and no room: a parser that still
// needs more data is desynced (NetMjpegSource's desync protocol).
//
// No I/O and no allocation: the socket side (poll + recv into tail()) is
// NetMjpegSource::net_fill; host tests drive the parsers through the same
// window (tests/test_multipart_reader.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

template <size_t N>
class ByteWindow
{
public:
    const uint8_t* data() const { return buf_ + pos_; }
    size_t size() const { return end_ - pos_; }
    size_t capacity() const { return N; }
    bool full() const { return size() == N; }

    // Drops n parsed bytes (clamped to size()).
    void consume( size_t n ) { pos_ += std::min( n, size() ); }
    // Drops everything: a desync, a new connection.
    void clear() { pos_ = end_ = 0; }

    // Moves the unconsumed bytes to the front and returns the free room
    // (room() bytes); commit( n ) appends n bytes written there.
    uint8_t* tail()
    {
        if ( pos_ > 0 )
        {
            std::memmove( buf_, buf_ + pos_, end_ - pos_ );
            end_ -= pos_;
            pos_ = 0;
        }
        return buf_ + end_;
    }
    size_t room() const { return N - end_; }
    void commit( size_t n ) { end_ += std::min( n, room() ); }

    // tail() + memcpy + commit(): the bytes that fit, returns how many.
    size_t append( const uint8_t* src, size_t n )
    {
        uint8_t* const dst = tail();
        const size_t k = std::min( n, room() );
        std::memcpy( dst, src, k );
        commit( k );
        return k;
    }

private:
    uint8_t buf_[N] = {};
    size_t pos_ = 0;
    size_t end_ = 0;
};
