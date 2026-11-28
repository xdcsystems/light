// SourceSet: the configured sources as one object -- it owns the channels and
// picks the next frame among them: a round-robin round over the Live channels
// from the cursor, with the 2 s stall detector.
//
// Threading (the SourceChannel thread rule, source/source_channel.hpp):
//   configure() -- the Pipeline ctor, before the prober starts;
//   next(), next_blocking(), reset_stall_clocks(), absent_mask() -- the frame
//     thread; they touch a channel's fields only while it is Live, the cursor
//     is the frame thread's own;
//   busy_nodes() -- the attaching thread only (Chan::node());
//   size(), operator[], any_live() -- any thread (size() is fixed after configure()).
//
// Template parameters (static polymorphism): Chan is the SourceChannel
// contract (source/source_channel.hpp); N is the most sources the run
// supports (one per NPU core); Make (ctor argument) is Chan( int index ),
// called once per channel, returning a prvalue (guaranteed elision: Chan
// need not be movable).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>

#include "config/config.hpp"  // SourceEndpoint, SourceKind
#include "core/log.hpp"
#include "core/non_copyable.hpp"
#include "source/source_types.hpp"

namespace source_detail
{
    // std::array of non-movable channels, each built in place by make( i ).
    template <class Chan, class Make, std::size_t... I>
    std::array<Chan, sizeof...( I )> make_channels( const Make& make, std::index_sequence<I...> /*unused*/ )
    {
        return { { make( static_cast<int>( I ) )... } };
    }
}  // namespace source_detail

template <class Chan, std::size_t N>
class BasicSourceSet : private NonMovable<BasicSourceSet<Chan, N>>
{
    using clk = std::chrono::steady_clock;

    enum
    {
        kStallMs = 2000,  // a Live source silent this long is treated as detached (EIO)
    };

public:
    template <class Make>
    BasicSourceSet( const Log& log, const Make& make )
      : log_( log ), ch_( source_detail::make_channels<Chan>( make, std::make_index_sequence<N> {} ) )
    {
    }
    ~BasicSourceSet() = default;

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

        // per-source decoders -- mixed resolutions (1280x960 USB next to
        // 1280x720 net) are supported.
        for ( std::size_t s = 0; s < eps.size(); ++s )
            ch_[s].configure( static_cast<int>( s ), eps[s], model_w, model_h );
        apply_cancel();
    }

    void set_stop( const std::atomic<bool>* stop ) noexcept
    {
        stop_ = stop;
        apply_cancel();
    }

    void set_wake_fd( int fd ) noexcept
    {
        wake_fd_ = fd;
        apply_cancel();
    }

    void set_prober_quit( const std::atomic<bool>* prober_quit ) noexcept
    {
        prober_quit_ = prober_quit;
        apply_cancel();
    }
    const std::atomic<bool>* prober_quit() const noexcept { return prober_quit_; }

    int size() const { return n_; }
    Chan& operator[]( int s ) { return ch_[static_cast<std::size_t>( s )]; }
    const Chan& operator[]( int s ) const { return ch_[static_cast<std::size_t>( s )]; }

    bool any_live() const
    {
        return std::any_of( ch_.begin(), ch_.begin() + n_, []( const Chan& c ) { return c.live(); } );
    }

    // One round over the Live channels, starting after the cursor (the source
    // of the last frame or failure). The first poll that is not NotReady wins
    // and moves the cursor. A NotReady channel without a frame for 2 s is
    // reported as Detached with out.st = dequeue_status( EIO ) (a stall: the source still
    // streams nothing). NotReady when the whole round had nothing.
    // Event-driven wait across all Live sources using poll(2). Returns false
    // if any Live source lacks a pollable fd (e.g. buffered net frame or test fake).
    bool wait_any( int timeout_ms ) noexcept
    {
        std::array<struct pollfd, N + 1> fds {};
        std::array<int, N> idx_map {};
        nfds_t nfds = 0;
        for ( int k = 0; k < n_; ++k )
        {
            const int s = ( cursor_ + 1 + k ) % n_;
            if ( !ch_[static_cast<std::size_t>( s )].live() )
                continue;
            const int fd = ch_[static_cast<std::size_t>( s )].poll_fd();
            if ( fd < 0 )
                return false;
            fds[nfds] = pollfd { fd, POLLIN, 0 };
            idx_map[nfds] = s;
            ++nfds;
        }
        if ( nfds == 0 )
            return false;
        nfds_t poll_cnt = nfds;
        if ( wake_fd_ >= 0 )
            fds[poll_cnt++] = pollfd { wake_fd_, POLLIN, 0 };
        const int rc = ::poll( fds.data(), poll_cnt, timeout_ms );
        if ( rc < 0 )
            return errno == EINTR;
        if ( rc == 0 || ( wake_fd_ >= 0 && ( fds[nfds].revents & ( POLLIN | POLLERR | POLLHUP | POLLNVAL ) ) != 0 ) )
            return true;
        for ( nfds_t i = 0; i < nfds; ++i )
        {
            if ( ( fds[i].revents & ( POLLIN | POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
            {
                cursor_ = ( idx_map[i] + n_ - 1 ) % n_;
                break;
            }
        }
        return true;
    }

    bool fd_not_ready( const Chan& c, int wait_ms ) const noexcept
    {
        const int fd = ( wait_ms == 0 ) ? c.poll_fd() : -1;
        if ( fd < 0 )
            return false;
        pollfd pfd { fd, POLLIN, 0 };
        return ::poll( &pfd, 1, 0 ) == 0;
    }

    bool stalled( int s, const Chan& c, EncodedFrame& out ) noexcept
    {
        if ( clk::now() - c.last_ok() < std::chrono::milliseconds( kStallMs ) )
            return false;
        log_.warn() << "source s" << s << ": no frame in " << static_cast<int>( kStallMs )
                    << " ms - treating as recoverable stall\n";
        cursor_ = s;
        out.st = dequeue_status( EIO );
        return true;
    }

    FrameStatus next( int wait_ms, EncodedFrame& out ) noexcept
    {
        for ( int k = 0; k < n_; ++k )
        {
            const int s = ( cursor_ + 1 + k ) % n_;
            Chan& c = ( *this )[s];
            if ( !c.live() )
                continue;
            if ( fd_not_ready( c, wait_ms ) )
            {
                if ( stalled( s, c, out ) )
                    return FrameStatus::Detached;
                continue;
            }
            const FrameStatus st = c.poll( wait_ms, out );
            if ( st != FrameStatus::NotReady )
            {
                cursor_ = s;
                return st;
            }
            ++eagain_cnt_[static_cast<std::size_t>( s )];
            if ( stalled( s, c, out ) )
                return FrameStatus::Detached;
        }
        return FrameStatus::NotReady;
    }

    // One source and an empty pool: the blocking dequeue on channel 0 (the
    // camera's own 2 s watchdog reports EIO; no stall detector here).
    // NotReady when channel 0 is not Live, or on EAGAIN / EINTR.
    FrameStatus next_blocking( EncodedFrame& out ) noexcept
    {
        cursor_ = 0;
        if ( n_ == 0 || !ch_[0].live() )
            return FrameStatus::NotReady;
        const FrameStatus st = ch_[0].poll( -1, out );
        if ( st == FrameStatus::NotReady )
            ++eagain_cnt_[0];
        return st;
    }

    uint32_t take_eagain_cnt( int s ) noexcept
    {
        const auto idx = static_cast<std::size_t>( s );
        const uint32_t v = eagain_cnt_[idx];
        eagain_cnt_[idx] = 0;
        return v;
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
    void apply_cancel() noexcept
    {
        for ( int s = 0; s < n_; ++s )
        {
            ch_[static_cast<std::size_t>( s )].set_cancel( stop_, prober_quit_ );
            ch_[static_cast<std::size_t>( s )].set_wake_fd( wake_fd_ );
        }
    }

private:
    const Log& log_;
    std::array<Chan, N> ch_;
    std::array<uint32_t, N> eagain_cnt_ {};
    const std::atomic<bool>* stop_ = nullptr;
    const std::atomic<bool>* prober_quit_ = nullptr;
    int wake_fd_ = -1;
    int n_ = 0;       // configured channels (<= N), fixed after configure()
    int cursor_ = 0;  // frame thread: the source of the last frame or failure
};
