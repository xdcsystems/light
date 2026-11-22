// TcpAvio: FFmpeg AVIOContext adapter over a POSIX TcpPullServer client socket.
// Lives in net/ so platform/ stays free of FFmpeg dependencies.

#pragma once

#include <cstdint>

#include "core/status.hpp"

class TcpPullServer;
struct AVFormatContext;
struct AVIOContext;

class TcpAvio
{
public:
    TcpAvio() noexcept = default;
    ~TcpAvio() { close(); }
    TcpAvio( const TcpAvio& ) = delete;
    TcpAvio& operator=( const TcpAvio& ) = delete;
    TcpAvio( TcpAvio&& ) = delete;
    TcpAvio& operator=( TcpAvio&& ) = delete;

    // Allocates a 32 KiB custom AVIOContext over tcp's accepted client socket
    // and attaches it to fc->pb (sets AVFMT_FLAG_CUSTOM_IO).
    // Status: Invalid (no client / no fc), NoMemory (av_malloc / avio_alloc_context).
    Status attach_avio( TcpPullServer& tcp, AVFormatContext* fc );

    // Frees the AVIOContext and its buffer; idempotent.
    void close() noexcept;

private:
    uint8_t* iobuf_ = nullptr;
    AVIOContext* pb_ = nullptr;
};
