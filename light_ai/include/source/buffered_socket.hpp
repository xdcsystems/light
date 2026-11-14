// BufferedSocket: one blocking TCP client socket and its receive window
// (core/byte_window.hpp). The connection part (resolve, a non-blocking
// connect with a 5 s window, TCP_NODELAY, send with MSG_NOSIGNAL) and the
// receive part (one poll + recv per fill, EINTR reported, not retried) of
// the net MJPEG source; the parsing stays with the caller (core/http_head.hpp,
// core/multipart_reader.hpp) over data()/size()/consume().
//
// POSIX only, no SDK: host-tested over a loopback listener
// (tests/test_buffered_socket.cpp). Thread: one owner at a time (the
// NetMjpegSource that holds it).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/byte_window.hpp"

class BufferedSocket
{
public:
    enum
    {
        kWindow = 256 * 1024,  // the receive window (the net source's desync limit)
        kConnectMs = 5000,
    };
    // One fill():
    enum class Fill : uint8_t
    {
        Data,         // bytes appended to the window
        Timeout,      // nothing within timeout_ms
        Interrupted,  // a signal interrupted poll/recv: transient, the caller retries
        Closed,       // EOF, a socket error, or no socket: the connection is gone
        Full          // the window holds kWindow unconsumed bytes: nothing was read
    };

public:
    BufferedSocket() = default;
    ~BufferedSocket() { close(); }
    BufferedSocket( const BufferedSocket& ) = delete;
    BufferedSocket& operator=( const BufferedSocket& ) = delete;
    BufferedSocket( BufferedSocket&& ) = delete;
    BufferedSocket& operator=( BufferedSocket&& ) = delete;

    // Closes what was open, then connects to host:port (IPv4). 0 = connected;
    // -1 = why (when non-null) has the reason: "addr resolution failed for <host>",
    // "socket: ...", "connect: ...", "connect: timeout"; the socket is closed.
    int connect( const std::string& host, int port, std::string* why );
    // Sends every byte (EINTR retried). 0 = sent; -1 = why: "send: <strerror>"
    // or "send: peer closed, 0 bytes sent".
    int send_all( const std::string& bytes, std::string* why ) const;
    // One poll( timeout_ms; < 0 = do not wait ) + one recv into the window.
    Fill fill( int timeout_ms );
    // Closes the socket and drops the window. Idempotent.
    void close();
    bool is_open() const { return fd_ >= 0; }

    // The window: the unconsumed bytes received so far.
    const uint8_t* data() const { return win_.data(); }
    size_t size() const { return win_.size(); }
    bool full() const { return win_.full(); }
    void consume( size_t n ) { win_.consume( n ); }
    void clear() { win_.clear(); }

private:
    int fd_ = -1;
    ByteWindow<kWindow> win_;
};
