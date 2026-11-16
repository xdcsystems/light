// Single-viewer TCP pull transport for libavformat muxers (NetEncoder, NetPreview).
// Owns the listening socket, the accepted client socket, and the custom AVIOContext.
#pragma once

#include <atomic>
#include <cstdint>

#include "core/status.hpp"

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
