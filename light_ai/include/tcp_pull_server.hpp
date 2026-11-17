// Single-viewer TCP pull transport for libavformat muxers (NetEncoder, NetPreview).
// Owns the listening socket, the accepted client socket, and the custom AVIOContext.
#pragma once

#include <atomic>
#include <cstdint>

#include "core/log.hpp"
#include "core/status.hpp"
#include "core/stop_flag.hpp"

struct AVFormatContext;
struct AVIOContext;

class TcpPullServer
{
public:
    TcpPullServer( int port, int sndbuf_bytes, int sndtimeo_sec );
    ~TcpPullServer();
    TcpPullServer( const TcpPullServer& ) = delete;
    TcpPullServer& operator=( const TcpPullServer& ) = delete;

    // Opens, binds (SO_REUSEADDR) and listens on port_. Status: Busy
    // (EADDRINUSE), Failed (any other socket/bind/listen errno).
    Status listen();

    // Polls lfd_ until a viewer connects or quit is set; applies
    // TCP_NODELAY, SO_SNDBUF and SO_SNDTIMEO on the accepted socket.
    // Status: Ok (client accepted), Timeout (quit / no viewer),
    // Failed (poll/accept errno), Invalid (no listening socket).
    Status accept_client( const std::atomic<bool>& quit );

    // Allocates a 32 KiB custom AVIOContext over the accepted client
    // socket and attaches it to fc->pb (sets AVFMT_FLAG_CUSTOM_IO).
    // Status: Invalid (no client / no fc), NoMemory (av_malloc /
    // avio_alloc_context).
    Status attach_avio( AVFormatContext* fc );

    // Tears down the current viewer AVIOContext and client socket;
    // keeps the listening socket open for the next viewer session.
    void close_client() noexcept;

    // Closes both the client session and the listening socket.
    void close_all() noexcept;

private:
    const int port_;
    const int sndbuf_bytes_;
    const int sndtimeo_sec_;
    int lfd_ = -1;
    int cfd_ = -1;
    uint8_t* iobuf_ = nullptr;
    AVIOContext* pb_ = nullptr;
};

// The serve-thread bind with a retry: every listen() failure is retried,
// not only EADDRINUSE -- the run keeps going without a viewer whatever the
// errno (the policy: recognition never sleeps for the output side; README,
// Run, "Output side and recognition"). Logging on state change only: one
// warn on the first failure, the success line is the caller's (its texts
// differ per side: enc / net).
// try_listen: one bind attempt, normally a lambda over TcpPullServer::listen.
// period_ms: the retry cadence, 2000 in production; tests pass less.
// Returns the last Status: Ok = bound; a failure = quit ended the wait, the
// caller returns and the output side sleeps.
template <typename ListenFn>
Status retry_listen( ListenFn try_listen, const std::atomic<bool>& quit, const Log& log,
                     const char* tag, int port, int period_ms = 2000 )
{
    Status st = try_listen();
    bool warned = false;
    while ( !st.ok() && !quit.load( std::memory_order_acquire ) )
    {
        if ( !warned )
        {
            const bool whole_s = period_ms % 1000 == 0;
            log.warn() << tag << ": tcp listener on port " << port << ": " << st
                       << "; retrying every " << ( whole_s ? period_ms / 1000 : period_ms )
                       << ( whole_s ? " s" : " ms" ) << '\n';
            warned = true;
        }
        if ( sleep_interruptible( quit, period_ms ) )
            break;
        st = try_listen();
    }
    return st;
}
