// Vocabulary of the source layer: what one dequeue returns, one encoded frame,
// one source's geometry, the busy-node list. SDK-free (host tests build it).

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/geometry.hpp"

// Outcome of one dequeue attempt. Replaces the old buffer index / -1 / -2
// codes with the error in an out-parameter.
enum class FrameStatus : uint8_t
{
    Frame,     // EncodedFrame filled: decode it, then the channel requeues the buffer
    NotReady,  // nothing within the wait (EAGAIN / EINTR): try another source or wait
    Detached,  // the source is gone (ENODEV / EIO, or a stall): the prober re-attaches it
    Fatal,     // any other errno: the run ends with exit code 1
    Stopped    // stop requested while waiting
};

// One dequeued MJPEG packet. src says which source it came from: it is passed
// along explicitly instead of a "current source" member.
struct EncodedFrame
{
    int src = -1;       // source index
    int buf = -1;       // the source's buffer index (requeued by SourceChannel::decode)
    uint32_t size = 0;  // payload bytes; 0 = an empty packet (skipped, never decoded)
    int err = 0;        // errno for Detached / Fatal (the failure log prints it)
};

// Geometry of a Live source; read by the frame thread after it sees Live.
struct SourceGeom
{
    int w = 0;
    int h = 0;
    LbGeom lb {};     // letterbox band of this source in the model input
    int aux_fd = -1;  // RGA intermediate for the two-pass path, -1 when not needed
};

// /dev nodes reserved by the other V4L2 sources. Built only on the attach path
// (startup and the prober), never per frame, so a vector is allowed here.
using NodeList = std::vector<std::string>;

inline bool node_listed( const NodeList& nodes, const std::string& node )
{
    return std::find( nodes.begin(), nodes.end(), node ) != nodes.end();
}
