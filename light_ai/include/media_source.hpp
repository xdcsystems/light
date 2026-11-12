#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "camera_v4l2.hpp"
#include "net_mjpeg_source.hpp"

// one concrete owner of the two source kinds (CameraV4L2, NetMjpegSource).
// The std::variant lives HERE, private: Pipeline never
// touches it -- every per-source call is a plain method, the with_src
// templates and call-site lambdas are gone. The contract is the
// identical method set both classes already expose (setup_device /
// start / dequeue / requeue / buffer / recover / width / height /
// teardown): each dispatch is a one-liner. Static dispatch only -- no
// virtuals, no heap, both alternatives live in-place by value (README).
class MediaSource
{
public:
    MediaSource() = default;
    ~MediaSource() = default;
    MediaSource( const MediaSource& ) = delete;
    MediaSource& operator=( const MediaSource& ) = delete;

    // One-time kind selection at init (the variant emplace<> of the old
    // init_one_source): default-constructs the chosen alternative in place.
    void use_camera() { src_.emplace<CameraV4L2>(); }
    void use_net() { src_.emplace<NetMjpegSource>(); }

    // Init-path escape hatches ONLY (not used in the frame loop):
    // camera node discovery (find_usb_capture_node) needs the concrete
    // CameraV4L2; the net source has no discovery step.
    CameraV4L2& camera() { return std::get<CameraV4L2>( src_ ); }
    NetMjpegSource& net() { return std::get<NetMjpegSource>( src_ ); }

    // 0 on success, non-zero with *why (both setups are non-throwing).
    int setup_device( const std::string& id, std::string* why )
    {
        return std::visit( [&]( auto& s ) { return s.setup_device( id, why ); }, src_ );
    }
    // CameraV4L2::start() throws on STREAMON failure (the pipeline's
    // bounded recover ladder catches it); NetMjpegSource::start() is a
    // no-op drain. The exception propagates unchanged.
    void start()
    {
        std::visit( []( auto& s ) { s.start(); }, src_ );
    }
    // Identical errno ladder on both kinds: EIO = recoverable stall,
    // ENODEV = closed by teardown, EAGAIN = nothing ready in timeout_ms
    // (timeout_ms < 0 = the blocking single-source mode, as before).
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
    // One recovery-ladder step: node discovery (camera) / reconnect (both).
    // busy_nodes: /dev nodes reserved by the other source slots (camera only).
    int recover( const std::string& id, std::string& new_id, const std::vector<std::string>& busy_nodes,
                 std::string* why )
    {
        return std::visit( [&]( auto& s ) { return s.recover( id, new_id, busy_nodes, why ); }, src_ );
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
    std::variant<CameraV4L2, NetMjpegSource> src_ { std::in_place_type<CameraV4L2> };
};
