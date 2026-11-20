#include <chrono>
#include <cstdint>

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/log.hpp"
#include "core/perf_clock.hpp"
#include "core/session_warmup.hpp"
#include "net_encoder.h"

namespace
{
    // The publish clock: one read per publish, used by the session warmup,
    // the pacer and as the VFR pts of a timestamped frame. The frame's
    // ts_us (its dequeue time, may be -1) is only checked for >= 0.
    int64_t steady_now_us()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch() ).count();
    }
}  // namespace

// Members in declaration order (net_encoder.h): the network layer, the
// frame ring, the composer (logs the stream layout), the codec (opens and
// logs), the queue, the gate, the serve session. The body starts the serve
// thread last, once everything it reaches is built; a throw anywhere
// before unwinds the members that exist, in reverse order.
NetEncoder::NetEncoder( const EncoderConfig& cfg, const Log& log )
  : cfg_( cfg )
  , ring_( cfg.layout )
  , composer_( cfg, log )
  , codec_( cfg, log )
  , pacer_( cfg.layout.nsrc, cfg.fps )
  , warmup_( cfg.layout.nsrc )
  , session_( cfg, queue_, gate_, log )
{
    session_.start();
}

// Batch C: the pipeline gates GPU box drawing on this (one call site:
// pipeline.cpp).
bool NetEncoder::viewer_active() const
{
    return gate_.active();
}

uint32_t NetEncoder::session_epoch() const noexcept
{
    return gate_.epoch();
}

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int64_t ts_us, int src_idx, bool defer_encode ) noexcept
{
    if ( !gate_.active() )
    {
        canvas_dirty_ = false;
        return;
    }
    // The session this publish belongs to: loaded after active(), so a
    // session seen live is seen with its own epoch (SessionGate::open
    // stores the epoch first). The pacer and a fail() use it.
    const uint32_t epoch = gate_.epoch();
    const int64_t now_us = steady_now_us();
    // A new session starts on a gray canvas and holds its first encode
    // until every source cell has a picture of this session, or 200 ms
    // (core/session_warmup.hpp). The cell is the one place_tile writes.
    if ( warmup_.enter( epoch, now_us ) )
    {
        composer_.reset_canvas();
        canvas_dirty_ = false;
    }
    const TilePlacement tp = place_tile( cfg_.layout, band, src_idx );
    warmup_.refreshed( tp.cell );
    // Multi-view: frames closer than the pacing step (or deferred because the
    // next completed NPU slot is already waiting in the FIFO) update their
    // tile in the canvas and arm canvas_dirty_ for flush_dirty().
    const bool warm_ready = warmup_.ready( now_us );
    const bool hold_for_peer = defer_encode && cfg_.layout.nsrc > 1;
    const bool do_enc = warm_ready && !hold_for_peer && pacer_.admit( now_us, queue_.depth(), epoch );
    if ( warm_ready )
        do_enc ? ++perf_.enc_admit : ++perf_.enc_skip;
    if ( do_enc )
    {
        canvas_dirty_ = false;
        ts_us_ = ( ts_us >= 0 ) ? now_us : -1;
        ring_.advance();
    }
    if ( !do_enc && warm_ready && cfg_.layout.nsrc > 1 )
    {
        canvas_dirty_ = true;
        dirty_ts_us_ = ts_us;
    }

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. In multi-view mode when two
    // cameras finish within a few ms of each other, or while the session
    // is held, !do_enc still updates the source's tile in the persistent
    // composite canvas (out = nullptr) without emitting an H.264 frame.
    const int slot = ring_.current();
    const auto cmp_t0 = std::chrono::steady_clock::now();
    const Status cst = composer_.compose( tensor_fd, tp, slot, do_enc ? ring_.buf( slot ) : nullptr );
    const double cmp_ms = ms_since( cmp_t0 );
    if ( cmp_ms > perf_.compose_ms_max )
        perf_.compose_ms_max = cmp_ms;
    if ( !cst.ok() )
    {
        canvas_dirty_ = false;
        queue_.fail( epoch );  // ends THIS session only: serve() reopens the stream path
        return;
    }
    if ( !do_enc )
        return;

    run_encoded( epoch, std::chrono::steady_clock::now() );
}

void NetEncoder::flush_dirty() noexcept
{
    if ( !canvas_dirty_ || cfg_.layout.nsrc <= 1 )
        return;
    if ( !gate_.active() )
    {
        canvas_dirty_ = false;
        return;
    }
    const uint32_t epoch = gate_.epoch();
    const int64_t now_us = steady_now_us();
    if ( warmup_.enter( epoch, now_us ) )
    {
        composer_.reset_canvas();
        canvas_dirty_ = false;
        return;
    }
    if ( !warmup_.ready( now_us ) || !pacer_.admit( now_us, queue_.depth(), epoch ) )
        return;
    canvas_dirty_ = false;
    ++perf_.enc_admit;
    ts_us_ = ( dirty_ts_us_ >= 0 ) ? now_us : -1;
    const int slot = ring_.advance();
    const auto cmp_t0 = std::chrono::steady_clock::now();
    const Status cst = composer_.flush_canvas( ring_.buf( slot ) );
    const double cmp_ms = ms_since( cmp_t0 );
    if ( cmp_ms > perf_.compose_ms_max )
        perf_.compose_ms_max = cmp_ms;
    if ( !cst.ok() )
    {
        queue_.fail( epoch );
        return;
    }
    run_encoded( epoch, std::chrono::steady_clock::now() );
}

// NO SIGNAL publish (idle): mirrors publish() -- encode under the gate,
// RGA outside it, error path wakes serve(). The caller paces idle
// encodes at a fixed fps; the pacer stamps them in the same budget as
// live ones: a live frame right after an idle one sees a fresh stamp and
// skips the duplicate encode (multi-view window).
// Session warmup as in publish(): the plate refreshes the absent cells;
// while the session is held (or the queue is busy) the tick updates the
// canvas only (multi-view), and the pacer is consulted only once the
// session is released. An idle tick can release the session.
// The frame counter and the timing stats never see idle frames.
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept
{
    const StreamLayout& lay = cfg_.layout;
    if ( !gate_.active() )
        return;
    const uint32_t epoch = gate_.epoch();  // see publish()
    const int64_t now_us = steady_now_us();
    if ( warmup_.enter( epoch, now_us ) )
    {
        composer_.reset_canvas();
        canvas_dirty_ = false;
    }
    bool any = false;
    for ( int s = 0; s < lay.nsrc; ++s )
    {
        if ( !tile_absent( absent, nabs, s ) )
            continue;
        any = true;
        warmup_.refreshed( s );  // the cell compose_idle() plates below
    }
    if ( !any )
        return;  // every tile went live since the tick was scheduled
    const bool warm_ready = warmup_.ready( now_us );
    const bool do_enc = warm_ready && pacer_.admit_idle( now_us, queue_.depth(), epoch );
    if ( warm_ready )
        do_enc ? ++perf_.idle_admit : ++perf_.idle_skip;
    if ( !do_enc )
    {
        // Held or the encode queue busy: no encode this tick. Multi-view
        // still plates the absent cells that lack it (none after the
        // first such tick: compose_idle skips plated cells).
        if ( lay.nsrc > 1 && !composer_.compose_idle( absent, nabs, ring_.current(), nullptr ).ok() )
            queue_.fail( epoch );
        return;
    }
    canvas_dirty_ = false;
    ts_us_ = ( ts_us >= 0 ) ? now_us : -1;
    const int slot = ring_.advance();
    const auto cmp_t0 = std::chrono::steady_clock::now();
    const Status ist = composer_.compose_idle( absent, nabs, slot, ring_.buf( slot ) );
    const double cmp_ms = ms_since( cmp_t0 );
    if ( cmp_ms > perf_.compose_ms_max )
        perf_.compose_ms_max = cmp_ms;
    if ( !ist.ok() )
    {
        queue_.fail( epoch );  // see publish()
        return;
    }
    run_encoded( epoch, std::chrono::steady_clock::now() );
}

void NetEncoder::run_encoded( uint32_t epoch, std::chrono::steady_clock::time_point now ) noexcept
{
    if ( has_last_enc_ && epoch == perf_epoch_ )
    {
        const double dt_ms = ms_between( last_enc_tp_, now );
        perf_.add_dt_enc( dt_ms );
    }
    last_enc_tp_ = now;
    has_last_enc_ = true;
    perf_epoch_ = epoch;

    const auto enc_t0 = std::chrono::steady_clock::now();
    // No session or epoch changed while cropping: drop the stale frame.
    gate_.run( epoch, [this]( const SessionGate::Session& s ) {
        if ( const Status est = codec_.encode( ring_.frame( ring_.current() ), ts_us_, s, queue_ ); !est.ok() )
            queue_.fail( s.epoch );
    } );
    const double h264_ms = ms_since( enc_t0 );
    if ( h264_ms > perf_.h264_ms_max )
        perf_.h264_ms_max = h264_ms;
}

EncPerfWindow NetEncoder::take_perf() noexcept
{
    const uint64_t ea = codec_.eagain_cnt();
    const uint64_t pc = codec_.pts_clamp_cnt();
    const uint64_t qf = codec_.q_flush_cnt();
    perf_.eagain_cnt = static_cast<uint32_t>( ea - prev_eagain_cnt_ );
    perf_.pts_clamp_cnt = static_cast<uint32_t>( pc - prev_pts_clamp_cnt_ );
    perf_.q_flush_cnt = static_cast<uint32_t>( qf - prev_q_flush_cnt_ );
    prev_eagain_cnt_ = ea;
    prev_pts_clamp_cnt_ = pc;
    prev_q_flush_cnt_ = qf;
    const EncPerfWindow out = perf_;
    perf_.reset();
    return out;
}

