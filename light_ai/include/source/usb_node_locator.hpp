// UsbNodeLocator: finds the V4L2 node of a USB camera -- the configured node
// first, then a /dev/videoN scan -- and never trials a node another source
// reserves (busy-node reservation: two sources must not compete for one camera).
//
// The scan-log throttle is a member, so every caller shares it.
//
// Threading: called only by the thread that attaches sources -- the Pipeline
// constructor before the prober starts, then the prober. No lock.
//
// Template parameter (static polymorphism): Host provides
//   exists( path ) -- the node is present (stat);
//   usb_capture_nodes( out ) -- numeric /dev/videoN nodes that are USB
//     capture devices (rkcif/rkisp platform nodes rejected), in any order,
//     returns the count (<= out.size());
//   now_ms() -- steady clock, for the throttle.
// Trial (locate() argument): Status( const std::string& node ),
//   ok = the full setup passed on that node; the trial leaves the camera closed.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "core/log.hpp"
#include "core/status.hpp"
#include "source/source_types.hpp"

enum
{
    kMaxCandidates = 64,         // /dev/videoN candidates per scan
    kScanLogThrottleMs = 30000,  // "scanning /dev/video*" at most every 30 s
};

// Pure V4L2 capability check: when V4L2_CAP_DEVICE_CAPS (0x80000000) is set,
// per-node capabilities live in device_caps rather than card-wide capabilities
// (rejects metadata nodes such as /dev/video21 early before S_FMT).
inline bool usb_cap_capture_streaming( uint32_t capabilities, uint32_t device_caps ) noexcept
{
    constexpr uint32_t video_capture = 0x00000001u;
    constexpr uint32_t streaming = 0x04000000u;
    constexpr uint32_t dev_caps = 0x80000000u;
    const uint32_t caps = ( capabilities & dev_caps ) != 0u ? device_caps : capabilities;
    return ( caps & video_capture ) != 0u && ( caps & streaming ) != 0u;
}

template <class Host>
class BasicUsbNodeLocator
{
public:
    BasicUsbNodeLocator( const Host& host, const Log& log ) : host_( host ), log_( log ) {}

    // The configured node when it is present, not busy and its trial passes;
    // otherwise the first scanned node (ascending N) that is not busy, not the
    // preferred one (it just failed above; the next attach retries it) and
    // whose trial passes. ok fills out; Absent "no usable USB capture node found"
    // when nothing is usable.
    template <class Trial>
    Status locate( const std::string& preferred, const NodeList& busy, Trial& trial, std::string& out )
    {
        if ( !preferred.empty() && !node_listed( busy, preferred ) && host_.exists( preferred ) )
        {
            const Status st = trial( preferred );
            if ( st.ok() )
            {
                out = preferred;
                return {};
            }
            log_.warn() << "camera: reconnect setup on " << preferred << " failed: " << st << '\n';
        }
        // The scan itself is never throttled, only its log line.
        const long long now = host_.now_ms();
        if ( last_scan_log_ms_ == 0 || now - last_scan_log_ms_ >= kScanLogThrottleMs )
        {
            log_.warn() << "camera: node " << preferred << " not present, scanning /dev/video*\n";
            last_scan_log_ms_ = now;
        }
        std::array<int, kMaxCandidates> cands {};
        const int nc = std::min( host_.usb_capture_nodes( cands ), static_cast<int>( cands.size() ) );
        std::sort( cands.begin(), cands.begin() + nc );
        for ( int k = 0; k < nc; ++k )
        {
            const std::string node = "/dev/video" + std::to_string( cands[static_cast<size_t>( k )] );
            if ( node_listed( busy, node ) )
                continue;  // reserved by another source: never trial-open it
            if ( node == preferred )
                continue;  // just failed the direct attempt above
            log_.warn() << "camera: trying " << node << '\n';
            if ( trial( node ).ok() )
            {
                out = node;
                return {};
            }
        }
        return make_status( Errc::Absent, "no usable USB capture node found" );
    }

    bool exists( const std::string& node ) const { return host_.exists( node ); }

private:
    const Host& host_;
    const Log& log_;
    long long last_scan_log_ms_ = 0;  // last "scanning" line; 0 = never printed
};
