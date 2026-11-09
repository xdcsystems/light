#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// The node is NOT part of the instance state: open/reconnect take it as
// an argument, the class owns only the fd/buffer machinery. Identity
// ("which node am I on") lives with the owner — Pipeline::cam_node_.
class CameraV4L2
{
    struct Buffer
    {
        void* start = nullptr;
        size_t length = 0;
    };

    enum : unsigned
    {
        kBufCount = 4,
        kDequeuePollMs = 2000,  // dequeue watchdog: no frame within this window = recoverable stall (EIO)
        kDefaultWidth = 1280,
        kDefaultHeight = 960
    };

public:
    CameraV4L2() = default;
    ~CameraV4L2();
    CameraV4L2( const CameraV4L2& ) = delete;
    CameraV4L2& operator=( const CameraV4L2& ) = delete;

    // Full open+verify path for a node:
    // open, S_FMT, G_FMT verification, REQBUFS, mmap, QBUF.
    // Nothing is trusted blindly: a substituted camera mode is caught here exactly like at startup. 
    // Non-throwing: failures return non-zero and fill *why (if not null); details are logged here.
    int setup_device( const std::string& dev, std::string* why );

    // Release the fd and all mappings; safe on a partially-open or already-dead device
    // (STREAMOFF errors are ignored on purpose).
    void teardown();

    // Disconnect recovery: tear the device down and re-open the given node
    // with the full setup path + STREAMON.
    // Returns 0 on success, non-zero on failure (the cause is logged here).
    int reconnect( const std::string& dev, std::string* why );

    // One recovery-ladder step, invoked by Pipeline::handle_source_failure
    // through the source variant:
    // node discovery (the node may have re-enumerated) + the full reconnect path.
    // A failed discovery is a failed attempt.
    // new_id: the adopted node path, unchanged when the discovery keeps id.
    int recover( const std::string& id, std::string& new_id, std::string* why );

    void start() const;

    // Returns the buffer index; size (if not null) receives the frame length.
    // On failure returns -1 and stores the errno value in *err:
    // ENODEV/EIO mean the device is gone or interrupted (recoverable);
    // any other errno is a hard error.
    // Corrupt buffers (BUF_FLAG_ERROR) are counted ACROSS calls --
    // the err_streak_ member, reset only by a clean frame or by reconnect();
    // kBufCount in a row = recoverable stall.
    int dequeue( uint32_t* size, int* err );
    void requeue( int index );
    void* buffer( int index ) const { return bufs_[index].start; }
    size_t buffer_size( int index ) const { return bufs_[index].length; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    // Non-throwing ioctl wrapper for the (re)connect path:
    // EINTR retry only, errno is the caller's error report.
    // xioctl keeps the throwing contract for start().
    int xioctl_r( unsigned long req, void* arg ) const;
    void xioctl( unsigned long req, void* arg ) const;

private:
    int fd_ = -1;
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    Buffer bufs_[kBufCount];
};

// Finds a usable USB capture node.
// Preferred: the configured path, when it exists and its full setup passes on cam;
// otherwise numeric /dev/video* candidates are filtered by the USB capture check
// (rkcif/rkisp platform nodes are rejected before any S_FMT is tried),
// and the first one whose setup passes wins.
// The trial always leaves the camera closed
// -- the caller re-opens the winning node via reconnect().
// Returns 0 and fills `out`; non-zero when nothing usable is found.

int find_usb_capture_node( CameraV4L2& cam, const std::string& preferred, std::string& out );
