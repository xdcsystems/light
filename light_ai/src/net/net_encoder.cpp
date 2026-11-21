#include <chrono>
#include <cstdint>

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/log.hpp"
#include "core/perf_clock.hpp"
#include "core/session_warmup.hpp"
#include "net/net_encoder.hpp"

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

// Members in declaration order (net_encoder.hpp): the network layer, the
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

// The first publish of a new session (any of the three publishers): the
// canvas starts gray, and a deferred multi-view frame of the previous
// session is dropped. True when this call started the session.
bool NetEncoder::enter_session( uint32_t epoch, int64_t now_us ) noexcept
{
    if ( !warmup_.enter( epoch, now_us ) )
        return false;
    composer_.reset_canvas();
    canvas_dirty_ = false;
    return true;
}

// compose() is one composer pass; its time (plus any prior RGA pass of the
// same frame, excluding GPU overlay draw) goes into the cmp_max window.
void NetEncoder::record_compose_ms( double cmp_ms ) noexcept
{
    if ( cmp_ms > perf_.compose_ms_max )
        perf_.compose_ms_max = cmp_ms;
}

template <typename Compose>
Status NetEncoder::timed_compose( Compose&& compose, double base_ms ) noexcept
{
    const auto t0 = PerfClock::now();
    const Status st = compose();
    record_compose_ms( base_ms + ms_since( t0 ) );
    return st;
}

// The encoded tail of every publisher, in this order: the canvas is no
// longer dirty, the codec timestamp (the publish time, or -1 for the 25 fps
// grid), the next ring slot, the timed compose( slot, out ) into it, then a
// failed compose ends THIS session only (serve() reopens the stream path),
// and a good one is encoded.
template <typename Compose>
void NetEncoder::compose_and_encode( uint32_t epoch, int64_t now_us, int64_t ts_flag, Compose&& compose,
                                     double prior_cmp_ms ) noexcept
{
    canvas_dirty_ = false;
    ts_us_ = ( ts_flag >= 0 ) ? now_us : -1;
    const int slot = ring_.advance();
    if ( !timed_compose( [&] { return compose( slot, ring_.buf( slot ) ); }, prior_cmp_ms ).ok() )
    {
        queue_.fail( epoch );
        return;
    }
    run_encoded( epoch, PerfClock::now() );
}

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int64_t ts_us, int src_idx, bool defer_encode,
                          OverlayDrawHook overlay ) noexcept
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
    enter_session( epoch, now_us );
    const TilePlacement tp = place_tile( cfg_.layout, band, src_idx );
    pacer_.observe_source( tp.cell, now_us );
    warmup_.refreshed( tp.cell );
    // Multi-view: frames closer than the pacing step (or deferred because the
    // next completed NPU slot is already waiting in the FIFO) update their
    // tile in the canvas and arm canvas_dirty_ for flush_dirty().
    const bool multi = cfg_.layout.nsrc > 1;
    const bool warm_ready = warmup_.ready( now_us );
    const bool hold_for_peer = defer_encode && multi;
    const bool do_enc = warm_ready && !hold_for_peer && pacer_.admit( now_us, queue_.depth(), epoch );
    if ( warm_ready )
        do_enc ? ++perf_.enc_admit : ++perf_.enc_skip;
    // Single view skipped by the pacer: no persistent multi-view canvas to
    // keep fresh, so skip both RGA passes and the GPU overlay draw.
    if ( !do_enc && !multi )
        return;

    // Step 1: RGA crops/scales the clean NPU tensor into the RGB888 output
    // canvas tile. Step 2: overlay draws 1:1 onto the canvas tile (GPU +
    // sync_cpu or CPU fallback). Step 3 (when do_enc): RGA converts canvas
    // RGB888 -> NV12 ring slot and h264_rkmpp encodes under the session gate.
    const auto t_tile = PerfClock::now();
    const Status tile_st = composer_.compose_tile( tensor_fd, tp );
    const double tile_ms = ms_since( t_tile );
    if ( !tile_st.ok() )
    {
        record_compose_ms( tile_ms );
        canvas_dirty_ = false;
        queue_.fail( epoch );
        return;
    }
    if ( overlay.fn != nullptr )
        overlay.fn( overlay.ctx, tp.dst );

    if ( do_enc )
    {
        compose_and_encode(
            epoch, now_us, ts_us, [this]( int /*slot*/, MppBuffer out ) { return composer_.flush_canvas( out ); },
            tile_ms );
        return;
    }
    record_compose_ms( tile_ms );
    if ( warm_ready )
    {
        canvas_dirty_ = true;
        dirty_ts_us_ = ts_us;
    }
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
    if ( enter_session( epoch, now_us ) )
        return;
    if ( !warmup_.ready( now_us ) || !pacer_.admit( now_us, queue_.depth(), epoch ) )
        return;
    ++perf_.enc_admit;
    compose_and_encode( epoch, now_us, dirty_ts_us_, [this]( int /*slot*/, MppBuffer out ) {
        return composer_.flush_canvas( out );
    } );
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
// Unlike publish() and flush_dirty(), no session leaves canvas_dirty_ as
// it is: harmless, a new session clears it in enter_session() first.
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs ) noexcept
{
    const StreamLayout& lay = cfg_.layout;
    if ( !gate_.active() )
        return;
    const uint32_t epoch = gate_.epoch();  // see publish()
    const int64_t now_us = steady_now_us();
    enter_session( epoch, now_us );
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
        // first such tick: compose_idle skips plated cells). Untimed.
        if ( lay.nsrc > 1 && !composer_.compose_idle( absent, nabs, ring_.current(), nullptr ).ok() )
            queue_.fail( epoch );
        return;
    }
    compose_and_encode( epoch, now_us, ts_us, [&]( int slot, MppBuffer out ) {
        return composer_.compose_idle( absent, nabs, slot, out );
    } );
}

void NetEncoder::run_encoded( uint32_t epoch, std::chrono::steady_clock::time_point now ) noexcept
{
    if ( epoch != perf_epoch_ )
        enc_lap_.reset();  // no interval across sessions
    perf_epoch_ = epoch;
    double dt_ms = 0.0;
    if ( enc_lap_.lap( now, dt_ms ) )
        perf_.add_dt_enc( dt_ms );

    const auto enc_t0 = PerfClock::now();
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
    perf_.eagain_cnt = eagain_.take( codec_.eagain_cnt() );
    perf_.pts_clamp_cnt = pts_clamp_.take( codec_.pts_clamp_cnt() );
    perf_.q_flush_cnt = q_flush_.take( codec_.q_flush_cnt() );
    const EncPerfWindow out = perf_;
    perf_.reset();
    return out;
}
