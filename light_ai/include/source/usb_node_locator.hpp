// UsbNodeLocator: finds the V4L2 node of a USB camera -- the configured node
// first, then a /dev/videoN scan -- and never trials a node another source
// reserves (busy-node reservation: two sources must not compete for one camera).
//
// Moved out of camera_v4l2.cpp (find_usb_capture_node) in stage 3a. The scan-log
// throttle is a member now (was Pipeline::scan_log_ms_ behind a long long*
// parameter that the recover() path left null), so every caller shares it.
//
// Threading: called only by the thread that attaches sources -- the Pipeline
// constructor before the prober starts, then the prober. No lock.
//
// Template parameter (static polymorphism; tests/test_usb_node_locator.cpp
// instantiates it with a fake host):
//   Host:
//     bool exists( const std::string& path ) const;   // the node is present (stat)
//     // numeric /dev/videoN nodes that are USB capture devices (rkcif/rkisp
//     // platform nodes rejected), in any order; returns the count (<= out.size()).
//     int usb_capture_nodes( std::array<int, kMaxCandidates>& out ) const;
//     long long now_ms() const;                         // steady clock, for the throttle
// Trial (locate() argument): int( const std::string& node, std::string* why ),
//   0 = the full setup passed on that node; the trial leaves the camera closed.

#pragma once

#include <algorithm>
#include <array>
#include <string>

#include "core/log.hpp"
#include "source/source_types.hpp"

enum
{
    kMaxCandidates = 64,         // /dev/videoN candidates per scan
    kScanLogThrottleMs = 30000,  // "scanning /dev/video*" at most every 30 s
};

template <class Host>
class BasicUsbNodeLocator
{
public:
    BasicUsbNodeLocator( const Host& host, const Log& log ) : host_( host ), log_( log ) {}

    // The configured node when it is present, not busy and its trial passes;
    // otherwise the first scanned node (ascending N) that is not busy, not the
    // preferred one (it just failed above; the next attach retries it) and
    // whose trial passes. Returns 0 and fills out, -1 when nothing is usable.
    template <class Trial>
    int locate( const std::string& preferred, const NodeList& busy, Trial&& trial, std::string& out )
    {
        std::string why;
        if ( !preferred.empty() && !node_listed( busy, preferred ) && host_.exists( preferred ) )
        {
            if ( trial( preferred, &why ) == 0 )
            {
                out = preferred;
                return 0;
            }
            log_.warn() << "camera: reconnect setup on " << preferred << " failed: " << why << '\n';
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
            if ( trial( node, &why ) == 0 )
            {
                out = node;
                return 0;
            }
        }
        return -1;
    }

    bool exists( const std::string& node ) const { return host_.exists( node ); }

private:
    const Host& host_;
    const Log& log_;
    long long last_scan_log_ms_ = 0;  // last "scanning" line; 0 = never printed
};
