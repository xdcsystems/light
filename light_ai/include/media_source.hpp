#pragma once
#include <cstdint>
#include <string>
#include <variant>

#include "camera_v4l2.hpp"
#include "core/log.hpp"
#include "net_mjpeg_source.hpp"

// One concrete owner of the two source kinds (CameraV4L2, NetMjpegSource).
// The std::variant lives HERE, private: SourceChannel never touches it --
// every per-source call is a plain method and each dispatch is a one-liner.
// Static dispatch only -- no virtuals, no heap, both alternatives live
// in-place by value (README). This is the Src of SourceChannel
// (source/source_channel.hpp lists the contract).
class MediaSource
{
public:
    explicit MediaSource( const Log& log ) : log_( log ), src_( std::in_place_type<CameraV4L2>, log ) {}
    ~MediaSource() = default;
    MediaSource( const MediaSource& ) = delete;
    MediaSource& operator=( const MediaSource& ) = delete;
    MediaSource( MediaSource&& ) = delete;
    MediaSource& operator=( MediaSource&& ) = delete;

    // One-time kind selection (SourceChannel::configure): constructs the
    // chosen alternative in place.
    void use_camera() { src_.emplace<CameraV4L2>( log_ ); }
    void use_net() { src_.emplace<NetMjpegSource>(); }

    // Starts streaming from id, tearing down whatever was open:
    //   camera -- reconnect(): teardown + full setup + STREAMON;
    //   net    -- setup_device() (connect, headers, first frame), then start():
    //             drains the frames queued in the socket, so the first
    //             dequeue is at the live edge.
    // 0 on success, non-zero with *why. Never throws.
    int open( const std::string& id, std::string* why )
    {
        if ( auto* cam = std::get_if<CameraV4L2>( &src_ ) )
            return cam->reconnect( id, why );
        auto& net = std::get<NetMjpegSource>( src_ );
        if ( net.setup_device( id, why ) != 0 )
            return -1;
        net.start();
        return 0;
    }
    // Node discovery trial (UsbNodeLocator): the full camera setup on node,
    // then teardown -- the camera is closed on every outcome. 0 = usable.
    // The net source has no discovery: always "not usable".
    int trial( const std::string& node, std::string* why )
    {
        auto* cam = std::get_if<CameraV4L2>( &src_ );
        if ( cam == nullptr )
            return -1;
        const int r = cam->setup_device( node, why );
        cam->teardown();
        return r;
    }
    // Identical errno ladder on both kinds: EIO = recoverable stall,
    // ENODEV = closed by teardown, EAGAIN = nothing ready in timeout_ms
    // (timeout_ms < 0 = the blocking single-source mode).
    int dequeue( uint32_t* size, int* err, int timeout_ms = -1 )
    {
        return std::visit( [&]( auto& s ) { return s.dequeue( size, err, timeout_ms ); }, src_ );
    }
    void requeue( int idx )
    {
        std::visit( [&]( auto& s ) { s.requeue( idx ); }, src_ );
    }
    // mmap'd MJPEG payload of the dequeued buffer (both: void* bufs_[i].start).
    void* buffer( int idx ) const
    {
        return std::visit( [&]( const auto& s ) { return s.buffer( idx ); }, src_ );
    }
    int width() const
    {
        return std::visit( []( const auto& s ) { return s.width(); }, src_ );
    }
    int height() const
    {
        return std::visit( []( const auto& s ) { return s.height(); }, src_ );
    }
    void teardown()
    {
        std::visit( []( auto& s ) { s.teardown(); }, src_ );
    }

private:
    const Log& log_;
    std::variant<CameraV4L2, NetMjpegSource> src_;
};
