// SourceSet: the configured sources as one object -- it owns the channels and
// picks the next frame among them: a round-robin round over the Live channels
// from the cursor, with the 2 s stall detector. Stage 3c: replaces
// Pipeline::init_source, try_dequeue_round, dequeue_next_source, cur_src_,
// nsrc_ and the busy_v4l2_nodes helper of stage 3b.
//
// Threading (the SourceChannel thread rule, source/source_channel.hpp):
//   configure() -- the Pipeline ctor, before the prober starts;
//   next(), next_blocking(), reset_stall_clocks(), absent_mask() -- the frame
//     thread; they touch a channel's fields only while it is Live, the cursor
//     is the frame thread's own;
//   busy_nodes() -- the attaching thread only (Chan::node());
//   size(), operator[], any_live() -- any thread (size() is fixed after configure()).
//
// Template parameters (static polymorphism; tests/test_source_set.cpp
// instantiates the set with SourceChannel over the fakes, pipeline.hpp with
// the real one):
//   Chan: the SourceChannel contract (source/source_channel.hpp):
//     void configure( int idx, const SourceEndpoint&, int model_w, int model_h );
//     bool live() const; SourceKind kind() const; const std::string& node() const;
//     FrameStatus poll( int wait_ms, EncodedFrame& );
//     time_point last_ok() const; void stamp_ok( time_point );   // steady_clock
//   N: the most sources the run supports (one per NPU core).
//   Make (ctor argument): Chan( int index ), called once per channel; returns a
//     prvalue (guaranteed elision: Chan need not be movable).

#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "config/config.hpp"  // SourceEndpoint, SourceKind
#include "core/log.hpp"
#include "source/source_types.hpp"

namespace source_detail
{
    // std::array of non-movable channels, each built in place by make( i ).
    template <class Chan, class Make, std::size_t... I>
    std::array<Chan, sizeof...( I )> make_channels( Make& make, std::index_sequence<I...> /*unused*/ )
    {
        return { { make( static_cast<int>( I ) )... } };
    }
}  // namespace source_detail

template <class Chan, std::size_t N>
class BasicSourceSet
{
    using clk = std::chrono::steady_clock;

    enum
    {
        kStallMs = 2000,  // a Live source silent this long is treated as detached (EIO)
    };

public:
    template <class Make>
    BasicSourceSet( const Log& log, Make&& make )
      : log_( log ), ch_( source_detail::make_channels<Chan>( make, std::make_index_sequence<N> {} ) )
    {
    }
    ~BasicSourceSet() = default;
    BasicSourceSet( const BasicSourceSet& ) = delete;
    BasicSourceSet& operator=( const BasicSourceSet& ) = delete;
    BasicSourceSet( BasicSourceSet&& ) = delete;
    BasicSourceSet& operator=( BasicSourceSet&& ) = delete;

    // The Pipeline ctor, once: one channel per endpoint, in order; no device is
    // opened. Throws when there are more endpoints than channels. An empty list
    // is a legitimate waiting-for-attach run (README).
    void configure( const std::vector<SourceEndpoint>& eps, int model_w, int model_h )
    {
        if ( eps.size() > N )
            throw std::runtime_error( "pipeline: at most " + std::to_string( N )
                                      + " sources are supported (one per NPU core)" );
        n_ = static_cast<int>( eps.size() );
        // the sleep loop itself lands with the udev patch
        if ( n_ == 0 )
            log_.info() << "sources configured: none - waiting for attach\n";
        // M5: per-source decoders -- mixed resolutions (1280x960 USB next to
        // 1280x720 net) are supported.
        for ( int s = 0; s < n_; ++s )
            ch_[static_cast<std::size_t>( s )].configure( s, eps[static_cast<std::size_t>( s )], model_w, model_h );
    }

    int size() const { return n_; }
    Chan& operator[]( int s ) { return ch_[static_cast<std::size_t>( s )]; }
    const Chan& operator[]( int s ) const { return ch_[static_cast<std::size_t>( s )]; }

    bool any_live() const
    {
        for ( int s = 0; s < n_; ++s )
        {
            if ( ( *this )[s].live() )
                return true;
        }
        return false;
    }

    // One round over the Live channels, starting after the cursor (the source
    // of the last frame or failure). The first poll that is not NotReady wins
    // and moves the cursor. A NotReady channel without a frame for 2 s is
    // reported as Detached with out.err = EIO (a stall: the source still
    // streams nothing). NotReady when the whole round had nothing.
    FrameStatus next( int wait_ms, EncodedFrame& out )
    {
        for ( int k = 0; k < n_; ++k )
        {
            const int s = ( cursor_ + 1 + k ) % n_;
            Chan& c = ( *this )[s];
            if ( !c.live() )
                continue;
            const FrameStatus st = c.poll( wait_ms, out );
            if ( st != FrameStatus::NotReady )
            {
                cursor_ = s;
                return st;
            }
            if ( clk::now() - c.last_ok() >= std::chrono::milliseconds( kStallMs ) )
            {
                log_.warn() << "source s" << s << ": no frame in " << static_cast<int>( kStallMs )
                            << " ms - treating as recoverable stall\n";
                cursor_ = s;
                out.err = EIO;
                return FrameStatus::Detached;
            }
        }
        return FrameStatus::NotReady;
    }

    // One source and an empty pool: the blocking dequeue on channel 0 (the
    // camera's own 2 s watchdog reports EIO; no stall detector here).
    // NotReady when channel 0 is not Live, or on EAGAIN / EINTR.
    FrameStatus next_blocking( EncodedFrame& out )
    {
        cursor_ = 0;
        if ( n_ == 0 || !ch_[0].live() )
            return FrameStatus::NotReady;
        return ch_[0].poll( -1, out );
    }

    // Restarts the stall clocks: the start of run(), and after a failure was
    // handled (the handling time must not count as silence). Every channel:
    // last_ok is atomic, and attach() stamps it again on Live anyway.
    void reset_stall_clocks( clk::time_point now )
    {
        for ( int s = 0; s < n_; ++s )
            ( *this )[s].stamp_ok( now );
    }

    // out[s] = channel s is configured and not Live; false past size().
    // Returns true when any configured channel is not Live.
    bool absent_mask( std::array<bool, N>& out ) const
    {
        bool any = false;
        for ( int s = 0; s < static_cast<int>( N ); ++s )
        {
            out[static_cast<std::size_t>( s )] = s < n_ && !( *this )[s].live();
            any = any || out[static_cast<std::size_t>( s )];
        }
        return any;
    }

    // Busy-node reservation (audit: multi-camera node hijacking): the /dev
    // nodes of the V4L2 channels other than self, Live or not alike. A
    // detached source still owns its node -- the prober re-attaches it on its
    // own cadence -- so a scanning source must never trial-open or adopt that
    // node: uvcvideo accepts the trial while the node is momentarily free, and
    // two sources would then compete for one physical camera.
    NodeList busy_nodes( int self ) const
    {
        NodeList busy;
        for ( int k = 0; k < n_; ++k )
        {
            const Chan& c = ( *this )[k];
            if ( k == self || c.kind() != SourceKind::V4L2 || c.node().empty() )
                continue;
            busy.push_back( c.node() );
        }
        return busy;
    }

private:
    const Log& log_;
    std::array<Chan, N> ch_;
    int n_ = 0;       // configured channels (<= N), fixed after configure()
    int cursor_ = 0;  // frame thread: the source of the last frame or failure
};
