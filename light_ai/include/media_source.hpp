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
    // Throws on a failed frame-pool mmap (startup only; the variant is left
    // valueless and only destroyed).
    void use_net() { src_.emplace<NetMjpegSource>( log_ ); }

    // Starts streaming from id, tearing down whatever was open:
    //   camera -- reconnect(): teardown + full setup + STREAMON;
    //   net    -- setup_device() (connect, headers, first frame), then start():
    //             drains the frames queued in the socket, so the first
    //             dequeue is at the live edge.
    // ok, or the reason. Never throws.
    Status open( const std::string& id )
    {
        if ( auto* cam = std::get_if<CameraV4L2>( &src_ ) )
            return cam->reconnect( id );
        auto& net = std::get<NetMjpegSource>( src_ );
        if ( Status st = net.setup_device( id ); !st.ok() )
            return st;
        net.start();
        return {};
    }
    // Node discovery trial (UsbNodeLocator): the full camera setup on node,
    // then teardown -- the camera is closed on every outcome. ok = usable.
    // The net source has no discovery: always "not usable".
    Status trial( const std::string& node )
    {
        auto* cam = std::get_if<CameraV4L2>( &src_ );
        if ( cam == nullptr )
            return make_status( Errc::Unsupported, "net source: no node discovery" );
        const Status st = cam->setup_device( node );
        cam->teardown();
        return st;
    }
    // Identical ladder on both kinds (dequeue_status, source/source_types.hpp):
    // Lost = a recoverable stall or closed by teardown, Timeout = nothing
    // ready in timeout_ms (timeout_ms < 0 = the blocking single-source mode).
    Status dequeue( int& index, uint32_t& size, int timeout_ms = -1 )
    {
        return std::visit( [&]( auto& s ) { return s.dequeue( index, size, timeout_ms ); }, src_ );
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
