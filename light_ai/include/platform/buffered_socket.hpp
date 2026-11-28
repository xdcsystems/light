// BufferedSocket: one blocking TCP client socket and its receive window
// (core/byte_window.hpp). The connection part (resolve, a non-blocking
// connect with a 5 s window, TCP_NODELAY, a send bounded by 5 s with
// MSG_NOSIGNAL) and the receive part (one poll + recv per fill, EINTR
// reported, not retried) of the net MJPEG source; every socket wait also polls
// the optional wake fd. The parsing stays with the caller (core/http_head.hpp,
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
#include "core/non_copyable.hpp"
#include "core/status.hpp"

class BufferedSocket : private NonMovable<BufferedSocket>
{
public:
    enum
    {
        kWindow = 256 * 1024,  // the receive window (the net source's desync limit)
        kConnectMs = 5000,
        kSendMs = 5000,
    };
    // One fill():
    enum class Fill : uint8_t
    {
        Data,         // bytes appended to the window
        Timeout,      // nothing within timeout_ms (or spurious POLLIN + EAGAIN)
        Interrupted,  // a signal interrupted poll/recv, or the wake fd is signalled: the caller retries or leaves on cancel
        Closed,       // EOF, a socket error, or no socket: the connection is gone
        Full          // the window holds kWindow unconsumed bytes: nothing was read
    };

public:
    BufferedSocket() = default;
    // Closes the socket, then gai_cancel()s a lookup still pending (see
    // connect()). Canceled or already done: no wait. EAI_NOTCANCELED: waits
    // for it, without a bound and ignoring cancel (see the definition).
    ~BufferedSocket();

    // Closes what was open, then connects to host:port (IPv4). A numeric host
    // resolves in place (AI_NUMERICHOST, no DNS); a hostname goes through
    // getaddrinfo_a, bounded by kConnectMs and polled in kConnectSliceMs steps
    // that observe cancel. The lookup is submitted from the calling thread: a
    // libanl worker created by a real-time caller starts as SCHED_OTHER only
    // because that policy is set with SCHED_RESET_ON_FORK
    // (pin_current_thread_rt). A lookup given up while glibc still runs it
    // (gai_cancel: EAI_NOTCANCELED) keeps its request in this object: the next
    // connect() first waits for it (same bound), the destructor without one.
    // The addresses of the lookup are tried in order (ai_next); one kConnectMs
    // deadline, started after the lookup, covers all of them. A Timeout (cancel
    // included) ends the walk: an address that never answers uses up the whole
    // deadline, only a faster failure moves on. Not ok: the socket is closed and
    // the Status says why (of the last address tried):
    //   Absent  -- "addr resolution failed"; "connect: <errno>" refused / unreachable;
    //   Timeout -- "connect: timeout" (kConnectMs); "connect: cancelled";
    //              "addr resolution: timeout" / "addr resolution: cancelled";
    //   Failed  -- "socket: <errno>", "connect: fcntl: <errno>", any other "connect: <errno>".
    Status connect( const std::string& host, int port );
    // Sends every byte: each non-blocking send (EINTR / EAGAIN retried) follows
    // a poll( POLLOUT ) in kConnectSliceMs steps that observe cancel; one
    // kSendMs deadline covers the whole call. Not ok: Timeout "send: timeout" /
    // "send: cancelled"; Lost "send: <errno>", "send: poll: <errno>",
    // "send: bad fd", "send: peer closed" or "send: peer closed, 0 bytes sent";
    // Failed "send: not connected".
    Status send_all( const std::string& bytes ) const;
    // Non-owning stop flags observed by connect(), send_all() and fill_until(). "Cancel" in this class = one of
    // these flags or the wake fd below; cancelled() reports the flags only.
    void set_cancel( const std::atomic<bool>* stop, const std::atomic<bool>* prober_quit = nullptr ) noexcept
    {
        stop_ = stop;
        prober_quit_ = prober_quit;
    }
    bool cancelled() const noexcept;
    // Non-owning, -1 = none: a level-triggered wake descriptor (StopFlag::wake_fd()) polled next to the socket and
    // never read. Readable (or broken) = cancel: it ends a socket wait at once, without a lost wakeup.
    void set_wake_fd( int fd ) noexcept { wake_fd_ = fd; }
    // One poll( timeout_ms; < 0 = do not wait ) + non-blocking recv (MSG_DONTWAIT) into the window.
    Fill fill( int timeout_ms );
    // Bounded wait across slice_ms steps, returning Interrupted promptly on cancel (a stop flag or the wake fd).
    Fill fill_until( int total_ms, int slice_ms = 200 );
    // Closes the socket and drops the window. Idempotent.
    void close();
    bool is_open() const { return fd_ >= 0; }

    // The window: the unconsumed bytes received so far.
    const uint8_t* data() const { return win_.data(); }
    size_t size() const { return win_.size(); }
    bool full() const { return win_.full(); }
    int fd() const noexcept { return fd_; }
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
    // The wake fd is readable or broken (a 0 ms poll); false without one.
    bool wake_signalled() const noexcept;
    bool is_cancelled() const noexcept { return cancelled() || wake_signalled(); }
    // One address: socket() + non-blocking connect() until deadline. Ok: fd_ is
    // connected and blocking again. Not ok: fd_ is closed.
    Status connect_one( const addrinfo& ai, std::chrono::steady_clock::time_point deadline );
    Status wait_connect( std::chrono::steady_clock::time_point deadline ) noexcept;
    // Ok once fd_ is writable; the failures of send_all().
    Status wait_writable( std::chrono::steady_clock::time_point deadline ) const noexcept;
    Status resolve( const std::string& host, const std::string& port_str, addrinfo*& res );
    // Waits for the pending lookup in kConnectSliceMs steps until deadline or
    // cancel (a stop flag or the wake fd, seen between the steps);
    // bounded = false: no deadline and no cancel (the destructor).
    // Returns its gai_error() result, EAI_INPROGRESS if it still runs.
    int await_lookup( std::chrono::steady_clock::time_point deadline, bool bounded ) noexcept;
    // Marks the finished lookup reaped and frees its result unless taken.
    void reap_lookup() noexcept;

private:
    int fd_ = -1;
    int wake_fd_ = -1;
    const std::atomic<bool>* stop_ = nullptr;
    const std::atomic<bool>* prober_quit_ = nullptr;
    Lookup lookup_;
    ByteWindow<kWindow> win_;
};
