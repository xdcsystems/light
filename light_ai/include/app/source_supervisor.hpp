// SourceSupervisor: owns UsbHost, UsbNodeLocator, and SourceSet; handles
// startup attach, NO SIGNAL parking while all sources are absent, and
// recoverable vs fatal source error recovery. Extracted from Pipeline.

#pragma once

#include <chrono>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "core/global_track.hpp"
#include "core/log.hpp"
#include "core/non_copyable.hpp"
#include "core/status.hpp"
#include "core/stop_flag.hpp"
#include "source/source_prober.hpp"
#include "source/source_set.hpp"
#include "source/source_types.hpp"
#include "source/sources.hpp"

class SourceSupervisor : private NonMovable<SourceSupervisor>
{
public:
    enum
    {
        kMaxSources = reid_cfg::kMaxSources
    };
    using SourceSet = BasicSourceSet<SourceChannel, kMaxSources>;
    using SourceProber = BasicSourceProber<SourceSet, UsbNodeLocator>;

public:
    SourceSupervisor( const StopFlag& stop, const Log& log );
    ~SourceSupervisor() = default;

    void configure( const std::vector<SourceEndpoint>& sources, int model_w, int model_h );
    void attach_initial( int model_w, int model_h );
    void reset_stall_clocks( std::chrono::steady_clock::time_point t0 ) noexcept;

    SourceSet& sources() noexcept { return srcs_; }
    const SourceSet& sources() const noexcept { return srcs_; }
    UsbNodeLocator& locator() noexcept { return locator_; }
    int size() const noexcept { return srcs_.size(); }
    int reconnects() const noexcept { return reconnects_; }
    SourceChannel& operator[]( int idx ) noexcept { return srcs_[idx]; }
    const SourceChannel& operator[]( int idx ) const noexcept { return srcs_[idx]; }

    // The next encoded frame or a failure; parks (NO SIGNAL, 40 ms quanta)
    // while no source is Live. Stopped once the stop flag is set.
    template <class Pool, class DrainFn, class IdleFn>
    FrameStatus next_frame( EncodedFrame& f, const Pool& pool, DrainFn&& drain_done, IdleFn&& idle_tick ) noexcept
    {
        const bool blocking = srcs_.size() == 1 && pool.empty();
        while ( !stop_.requested() )
        {
            drain_done();
            idle_tick();  // mixed state: absent tiles keep publishing NO SIGNAL
            if ( !srcs_.any_live() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
                continue;
            }
            if ( pool.size() > 1 && pool.full() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                continue;
            }
            const FrameStatus st = blocking ? srcs_.next_blocking( f ) : srcs_.next( 0, f );
            if ( st != FrameStatus::NotReady )
                return st;
            if ( blocking )
                continue;
            drain_done();
            idle_tick();
            if ( !srcs_.wait_any( 20 ) )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        return FrameStatus::Stopped;
    }

    // Disconnect policy: Errc::Lost (ENODEV/EIO) detaches the channel for the
    // SourceProber to re-attach; any other error is fatal (returned to caller).
    // A recoverable failure that names no channel (f.src outside [0, size())) is
    // fatal too: Errc::Failed, nothing is discarded or detached.
    template <class Pool, class ResetFn>
    Status handle_failure( const EncodedFrame& f, Pool& pool, ResetFn&& reset_source ) noexcept
    {
        const bool recoverable = f.st.code() == Errc::Lost;
        log_.warn() << "source: " << f.st << ( recoverable ? " (recoverable)" : " (fatal)" ) << '\n';
        if ( !recoverable )
            return f.st;
        if ( f.src < 0 || f.src >= srcs_.size() )
        {
            log_.error() << "source: invalid channel index " << f.src << " (" << srcs_.size() << " configured)\n";
            return make_status( Errc::Failed, "source: invalid channel index" );
        }
        pool.discard( f.src );
        ++reconnects_;
        srcs_[f.src].detach();
        reset_source( f.src );
        log_.info() << "source s" << f.src << ": detached - the prober will re-attach\n";
        return {};
    }

private:
    const StopFlag& stop_;
    const Log& log_;
    UsbHost usb_host_;
    UsbNodeLocator locator_;
    SourceSet srcs_;
    int reconnects_ = 0;
};
