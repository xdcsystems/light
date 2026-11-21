// Vocabulary of the source layer: what one dequeue returns, one encoded frame,
// one source's geometry, the busy-node list. SDK-free (host tests build it).

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "core/geometry.hpp"
#include "core/status.hpp"

// Outcome of one dequeue attempt (SourceChannel::poll, from the Status of the
// device's dequeue; the failure itself is in EncodedFrame::st).
enum class FrameStatus : uint8_t
{
    Frame,     // EncodedFrame filled: decode it, then the channel requeues the buffer
    NotReady,  // Errc::Timeout -- nothing within the wait (EAGAIN / EINTR): try another source or wait
    Detached,  // Errc::Lost -- the source is gone (ENODEV / EIO, or a stall): the prober re-attaches it
    Fatal,     // any other Status: the run ends with exit code 1
    Stopped    // stop requested while waiting
};

// The errno ladder of a device dequeue, the same for both kinds (camera, net),
// as a Status "dequeue failed: <errno>":
//   EAGAIN, EINTR -- Timeout (not ready);
//   ENODEV, EIO   -- Lost (gone, or a recoverable stall);
//   anything else -- Failed (fatal).
inline Status dequeue_status( int err )
{
    Errc c = Errc::Failed;
    switch ( err )
    {
    case EAGAIN:
    case EINTR:
        c = Errc::Timeout;
        break;
    case ENODEV:
    case EIO:
        c = Errc::Lost;
        break;
    default:
        break;
    }
    return errno_status( c, "dequeue failed", err );
}

// One dequeued MJPEG packet. src says which source it came from: it is passed
// along explicitly instead of a "current source" member.
struct EncodedFrame
{
    int src = -1;       // source index
    int buf = -1;       // the source's buffer index (requeued by SourceChannel::decode)
    uint32_t size = 0;  // payload bytes; 0 = an empty packet (skipped, never decoded)
    int64_t cap_us = -1;  // monotonic capture timestamp (us), -1 if unavailable
    Status st {};       // Detached / Fatal: the failure (the failure log prints it)
};

// Geometry of a Live source; read by the frame thread after it sees Live.
struct SourceGeom
{
    int w = 0;        // effective scene width (swapped on 90/270 rot)
    int h = 0;        // effective scene height
    LbGeom lb {};     // letterbox band of this source in the model input
    int aux_fd = -1;  // RGA intermediate for the two-pass path, -1 when not needed
    int raw_w = 0;    // raw decoder width before RGA rotation
    int raw_h = 0;    // raw decoder height before RGA rotation
    int rot_deg = 0;  // configured RGA rotation (0/90/180/270)
};

// /dev nodes reserved by the other V4L2 sources. Built only on the attach path
// (startup and the prober), never per frame, so a vector is allowed here.
using NodeList = std::vector<std::string>;

inline bool node_listed( const NodeList& nodes, const std::string& node )
{
    return std::find( nodes.begin(), nodes.end(), node ) != nodes.end();
}
