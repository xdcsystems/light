#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <variant>

#include "source/camera_v4l2.hpp"
#include "core/log.hpp"
#include "source/net_mjpeg_source.hpp"

// Dispatch: the uniform per-kind methods (dequeue, requeue, buffer,
// width, height, teardown) use std::visit with a generic lambda --
// alternatives are derived, not named, so a new source kind edits the
// variant list only. visit throws bad_variant_access on a valueless
// variant (reachable only after a failed use_net() emplace, startup
// only); the narrow catch in each dispatch maps it to the method's
// no-object result -- the same outcome the explicit ladders produced.
// open()/trial() stay explicit get_if: their flows differ per kind.

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
    void set_cancel( const std::atomic<bool>* stop, const std::atomic<bool>* prober_quit ) noexcept
    {
        if ( auto* net = std::get_if<NetMjpegSource>( &src_ ) )
            net->set_cancel( stop, prober_quit );
    }
    void set_wake_fd( int fd ) noexcept
    {
        if ( auto* cam = std::get_if<CameraV4L2>( &src_ ) )
            cam->set_wake_fd( fd );
    }

    // Starts streaming from id, tearing down whatever was open:
    //   camera -- reconnect(): teardown + full setup + STREAMON;
    //   net    -- setup_device() (connect, headers, first frame), then start():
    //             drains the frames queued in the socket, so the first
    //             dequeue is at the live edge.
    // ok, or the reason. Never throws. (Explicit get_if: per-kind flow.)
    Status open( const std::string& id )
    {
        if ( auto* cam = std::get_if<CameraV4L2>( &src_ ) )
            return cam->reconnect( id );
        auto* net = std::get_if<NetMjpegSource>( &src_ );
        if ( net == nullptr )  // only after a failed use_net() emplace: never thrown into the frame path
            return make_status( Errc::Unsupported, "source: variant is valueless" );
        if ( Status st = net->setup_device( id ); !st.ok() )
            return st;
        net->start();
        return {};
    }
    // Node discovery trial (UsbNodeLocator): the full camera setup on node,
    // then teardown -- the camera is closed on every outcome. ok = usable.
    // The net source has no discovery: always "not usable". (Explicit
    // get_if: per-kind flow.)
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
    Status dequeue( int& index, uint32_t& size, int timeout_ms = -1 ) noexcept
    {
        try
        {
            return std::visit( [&]( auto& s ) { return s.dequeue( index, size, timeout_ms ); }, src_ );
        }
        catch ( const std::bad_variant_access& )  // only after a failed use_net() emplace (startup only)
        {
            return make_status( Errc::Unsupported, "source: variant is valueless" );
        }
    }
    void requeue( int idx ) noexcept
    {
        try
        {
            std::visit( [&]( auto& s ) { s.requeue( idx ); }, src_ );
        }
        catch ( const std::bad_variant_access& ) {}  // only after a failed use_net() emplace
    }
    // mmap'd MJPEG payload of the dequeued buffer (both: void* bufs_[i].start).
    int poll_fd() const noexcept
    {
        try
        {
            return std::visit( []( const auto& s ) noexcept { return s.poll_fd(); }, src_ );
        }
        catch ( const std::bad_variant_access& )
        {
            return -1;
        }
    }
    void* buffer( int idx ) const noexcept
    {
        try
        {
            return std::visit( [&]( const auto& s ) { return s.buffer( idx ); }, src_ );
        }
        catch ( const std::bad_variant_access& )  // only after a failed use_net() emplace
        {
            return nullptr;
        }
    }
    int width() const
    {
        try
        {
            return std::visit( []( const auto& s ) { return s.width(); }, src_ );
        }
        catch ( const std::bad_variant_access& )  // only after a failed use_net() emplace
        {
            return -1;
        }
    }
    int height() const
    {
        try
        {
            return std::visit( []( const auto& s ) { return s.height(); }, src_ );
        }
        catch ( const std::bad_variant_access& )  // only after a failed use_net() emplace
        {
            return -1;
        }
    }
    int64_t last_cap_us() const noexcept
    {
        try
        {
            return std::visit( []( const auto& s ) { return s.last_cap_us(); }, src_ );
        }
        catch ( const std::bad_variant_access& )
        {
            return -1;
        }
    }
    uint64_t skip_parts_cnt() const noexcept
    {
        try
        {
            return std::visit( []( const auto& s ) { return s.skip_parts_cnt(); }, src_ );
        }
        catch ( const std::bad_variant_access& )  // only after a failed use_net() emplace
        {
            return 0;
        }
    }
    void teardown() noexcept
    {
        try
        {
            std::visit( []( auto& s ) { s.teardown(); }, src_ );
        }
        catch ( const std::bad_variant_access& ) {}  // only after a failed use_net() emplace
    }

private:
    const Log& log_;
    std::variant<CameraV4L2, NetMjpegSource> src_;
};
