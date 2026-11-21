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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <netdb.h>

#include "core/byte_window.hpp"
#include "core/status.hpp"

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
    // Closes the socket, then waits for a hostname lookup that glibc could not
    // cancel (see connect()): its request lives in this object.
    ~BufferedSocket();
    BufferedSocket( const BufferedSocket& ) = delete;
    BufferedSocket& operator=( const BufferedSocket& ) = delete;
    BufferedSocket( BufferedSocket&& ) = delete;
    BufferedSocket& operator=( BufferedSocket&& ) = delete;

    // Closes what was open, then connects to host:port (IPv4). A numeric host
    // resolves in place (AI_NUMERICHOST, no DNS); a hostname goes through
    // getaddrinfo_a, bounded by kConnectMs and polled in kConnectSliceMs steps
    // that observe cancelled(). A lookup given up while glibc still runs it
    // (gai_cancel: EAI_NOTCANCELED) keeps its request in this object: the next
    // connect() first waits for it (same bound), the destructor waits for it
    // without one. Not ok: the socket is closed and the Status says why:
    //   Absent  -- "addr resolution failed"; "connect: <errno>" refused / unreachable;
    //   Timeout -- "connect: timeout" (kConnectMs); "connect: cancelled";
    //              "addr resolution: timeout" / "addr resolution: cancelled";
    //   Failed  -- "socket: <errno>", "connect: fcntl: <errno>", any other "connect: <errno>".
    Status connect( const std::string& host, int port );
    // Sends every byte (EINTR retried). Not ok: Lost "send: <errno>" or
    // "send: peer closed, 0 bytes sent"; Failed "send: not connected".
    Status send_all( const std::string& bytes ) const;
    // Non-owning stop flags observed by connect() and fill_until().
    void set_cancel( const std::atomic<bool>* stop, const std::atomic<bool>* prober_quit = nullptr ) noexcept
    {
        stop_ = stop;
        prober_quit_ = prober_quit;
    }
    bool cancelled() const noexcept;
    // One poll( timeout_ms; < 0 = do not wait ) + one recv into the window.
    Fill fill( int timeout_ms );
    // Bounded wait across slice_ms steps, returning Interrupted promptly on cancel.
    Fill fill_until( int total_ms, int slice_ms = 200 );
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
    // A hostname lookup through getaddrinfo_a. The request (gaicb, hints and
    // both strings) is read and written by glibc until gai_error() stops
    // returning EAI_INPROGRESS; pending marks a request not yet reaped.
    struct Lookup
    {
        gaicb cb {};
        addrinfo hints {};
        std::string name;
        std::string service;
        bool pending = false;
    };

private:
    Status wait_connect() noexcept;
    Status resolve( const std::string& host, const std::string& port_str, addrinfo*& res );
    // Waits for the pending lookup in kConnectSliceMs steps until deadline or
    // cancelled(); bounded = false: no deadline and no cancel (the destructor).
    // Returns its gai_error() result, EAI_INPROGRESS if it still runs.
    int await_lookup( std::chrono::steady_clock::time_point deadline, bool bounded ) noexcept;
    // Marks the finished lookup reaped and frees its result unless taken.
    void reap_lookup() noexcept;

private:
    int fd_ = -1;
    const std::atomic<bool>* stop_ = nullptr;
    const std::atomic<bool>* prober_quit_ = nullptr;
    Lookup lookup_;
    ByteWindow<kWindow> win_;
};
