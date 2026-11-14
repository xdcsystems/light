// The concrete source layer: the templates of source/ instantiated with the
// real device types (explicit instantiations in src/sources.cpp).

#pragma once

#include <array>
#include <string>

#include "media_source.hpp"
#include "mpp_decoder.hpp"
#include "source/source_channel.hpp"
#include "source/usb_node_locator.hpp"

// The Host of UsbNodeLocator on this board: /dev, the V4L2 QUERYCAP check, the steady clock.
class UsbHost
{
public:
    bool exists( const std::string& path ) const;
    // Numeric /dev/videoN nodes that are V4L2 capture + streaming devices on a
    // USB bus (QUERYCAP bus_info contains "usb"). Everything else on this board --
    // rkcif/rkisp platform nodes -- is rejected before any S_FMT is tried: they
    // would burn attach attempts on setup failures, and a platform capture node
    // might accept the format while never delivering a frame (no sensor attached).
    int usb_capture_nodes( std::array<int, kMaxCandidates>& out ) const;
    long long now_ms() const;
};

using UsbNodeLocator = BasicUsbNodeLocator<UsbHost>;
using SourceChannel = BasicSourceChannel<MediaSource, MppDecoder>;

extern template class BasicUsbNodeLocator<UsbHost>;
extern template class BasicSourceChannel<MediaSource, MppDecoder>;
