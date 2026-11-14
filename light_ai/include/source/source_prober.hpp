// SourceProber: attaches the non-Live sources on its own thread, on a 2 s
// cadence, with a throttled "absent" log line. Stage 3b: replaces
// Pipeline::probe_loop, probe_source, busy_v4l2_nodes, the ProbeStop guard and
// the probe_th_ / probe_quit_ / probe_ticks_ members.
//
// One thread for all sources: a net attach may block for the connect timeout
// (5 s) -- it must not stall the frame loop, hence a dedicated thread instead
// of probing from the frame loop. V4L2 attach latency <= ~2 s, net <= ~7 s.
//
// Lifetime (RAII): the Pipeline keeps the prober as a local of run(), so the
// destructor stops and joins it on every exit of run() -- drain, fatal error,
// exception unwind -- before any channel dies. start() must be called BEFORE
// the calling thread pins itself to the big cores: the prober inherits the
// creator's mask and has to keep the scheduler default (cpu_affinity.hpp).
//
// Threading: from start() until the join the prober thread is the attaching
// thread of the SourceChannel thread rule (source/source_channel.hpp): it
// reads state() (acquire) and calls attach() only on non-Live channels, and
// it is the only user of the locator. stop() may be called from any thread.
//
// Template parameters (static polymorphism; tests/test_source_prober.cpp
// instantiates it with SourceChannel over the fakes, pipeline.hpp with the real one):
//   Chan: the SourceChannel contract (source/source_channel.hpp):
//     bool live() const; SourceKind kind() const; const std::string& node() const;
//     template <class Loc> Attach attach( const NodeList& busy, Loc&, bool hot, std::string& why );
//   N: the channel array size; only the first n (ctor) channels are probed.
//   Locator: passed through to Chan::attach (source/usb_node_locator.hpp).

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "config/config.hpp"  // SourceKind
#include "core/log.hpp"
#include "source/source_types.hpp"

// Busy-node reservation (audit: multi-camera node hijacking): the /dev nodes
// of the V4L2 channels other than self among the first n, Live or not alike.
// A detached source still owns its node -- the prober re-attaches it on its
// own cadence -- so a scanning source must never trial-open or adopt that
// node: uvcvideo accepts the trial while the node is momentarily free, and two
// sources would then compete for one physical camera.
// Attaching thread only (Chan::node()).
template <class Chan, std::size_t N>
NodeList busy_v4l2_nodes( const std::array<Chan, N>& chans, int n, int self )
{
    NodeList busy;
    for ( int k = 0; k < n; ++k )
    {
        const Chan& c = chans[static_cast<std::size_t>( k )];
        if ( k == self || c.kind() != SourceKind::V4L2 || c.node().empty() )
            continue;
        busy.push_back( c.node() );
    }
    return busy;
}

template <class Chan, std::size_t N, class Locator>
class BasicSourceProber
{
public:
    enum
    {
        kPeriodMs = 2000,  // one round every 2 s
        kLogEvery = 15,    // "absent" line on the 1st and every 15th round (~30 s)
    };

    // chans: the channels; the first n are probed (n <= N). period_ms: the
    // cadence -- the default in production, short in the threaded tests.
    BasicSourceProber( std::array<Chan, N>& chans, int n, Locator& loc, const Log& log, int period_ms = kPeriodMs )
      : chans_( chans ), n_( n < static_cast<int>( N ) ? n : static_cast<int>( N ) ), loc_( loc ), log_( log )
      , period_( period_ms )
    {
    }
    ~BasicSourceProber() { stop(); }
    BasicSourceProber( const BasicSourceProber& ) = delete;
    BasicSourceProber& operator=( const BasicSourceProber& ) = delete;
    BasicSourceProber( BasicSourceProber&& ) = delete;
    BasicSourceProber& operator=( BasicSourceProber&& ) = delete;

    // Spawns the prober thread: the first round runs at once, then one round
    // every period. No thread with no sources, or when already started.
    void start()
    {
        if ( n_ <= 0 || th_.joinable() )
            return;
        th_ = std::thread( [this] { loop(); } );
    }

    // Wakes the prober and joins it. Returns at once between rounds; during a
    // round it waits for the attach in progress (a net connect: up to 5 s).
    // Idempotent; any thread but the prober's.
    void stop()
    {
        {
            const std::lock_guard<std::mutex> lk( mu_ );
            quit_ = true;
        }
        cv_.notify_all();
        if ( th_.joinable() )
            th_.join();
    }

    // One round: attaches every non-Live channel once, never throws on an
    // attach failure (the channel stays non-Live, the next round retries the
    // same path). The prober thread; the tests call it directly, unstarted.
    void round()
    {
        ++rounds_;
        for ( int s = 0; s < n_; ++s )
        {
            Chan& c = chans_[static_cast<std::size_t>( s )];
            if ( quitting() || c.live() )
                continue;
            // visibility: the 1st and every 15th round show the prober is
            // alive; the camera scan prints on its own (UsbNodeLocator).
            if ( rounds_ == 1 || rounds_ % kLogEvery == 0 )
                log_.info() << "probe: s" << s << " absent, attempt " << rounds_ << '\n';
            std::string why;
            static_cast<void>( c.attach( busy_v4l2_nodes( chans_, n_, s ), loc_, true, why ) );
        }
    }

    // Rounds run so far (prober thread, or the test after the join).
    uint64_t rounds() const { return rounds_; }

private:
    void loop()
    {
        std::unique_lock<std::mutex> lk( mu_ );
        while ( !quit_ )
        {
            lk.unlock();  // a round never holds the lock: stop() must not wait for it
            round();
            lk.lock();
            cv_.wait_for( lk, period_, [this] { return quit_; } );
        }
    }

    bool quitting()
    {
        const std::lock_guard<std::mutex> lk( mu_ );
        return quit_;
    }

private:
    std::array<Chan, N>& chans_;
    const int n_;
    Locator& loc_;
    const Log& log_;
    const std::chrono::milliseconds period_;
    uint64_t rounds_ = 0;  // prober thread only: throttles the "absent" line

    std::mutex mu_;  // guards quit_; cv_ wakes the wait between rounds
    std::condition_variable cv_;
    bool quit_ = false;
    std::thread th_;
};
