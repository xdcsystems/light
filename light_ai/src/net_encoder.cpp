#include <chrono>
#include <cstdint>

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/log.hpp"
#include "core/session_warmup.hpp"
#include "net_encoder.h"

namespace
{
    // The session warmup clock: one read per publish, independent of the
    // frame's ts_us (a live frame's dequeue time lags "now" by the pipeline
    // latency, and ts_us may be -1).
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

void NetEncoder::publish( int tensor_fd, const LbGeom& band, int64_t ts_us, int src_idx )
{
    if ( !gate_.active() )
        return;
    // The session this publish belongs to: loaded after active(), so a
    // session seen live is seen with its own epoch (SessionGate::open
    // stores the epoch first). The pacer and a fail() use it.
    const uint32_t epoch = gate_.epoch();
    const int64_t now_us = steady_now_us();
    // A new session starts on a gray canvas and holds its first encode
    // until every source cell has a picture of this session, or 200 ms
    // (core/session_warmup.hpp). The cell is the one place_tile writes.
    if ( warmup_.enter( epoch, now_us ) )
        composer_.reset_canvas();
    const TilePlacement tp = place_tile( cfg_.layout, band, src_idx );
    warmup_.refreshed( tp.cell );
    // Multi-view: frames closer than the pacing step only update their
    // tile in the canvas (core/encode_pacer.hpp). The pacer is not
    // consulted while the session is held: the releasing frame is the
    // session's first for it.
    const bool do_enc = warmup_.ready( now_us ) && pacer_.admit( ts_us, queue_.depth(), epoch );
    if ( do_enc )
    {
        ts_us_ = ts_us;
        ring_.advance();
    }

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. In multi-view mode when two
    // cameras finish within a few ms of each other, or while the session
    // is held, !do_enc still updates the source's tile in the persistent
    // composite canvas (out = nullptr) without emitting an H.264 frame.
    const int slot = ring_.current();
    if ( !composer_.compose( tensor_fd, tp, slot, do_enc ? ring_.buf( slot ) : nullptr ) )
    {
        queue_.fail( epoch );  // ends THIS session only: serve() reopens the stream path
        return;
    }
    if ( !do_enc )
        return;

    // No session any more (viewer dropped while cropping): the frame is
    // dropped with it. A send failure ends the session it was sent into.
    gate_.run( [this]( const SessionGate::Session& s ) {
        if ( !codec_.encode( ring_.frame( ring_.current() ), ts_us_, s, queue_ ) )
            queue_.fail( s.epoch );
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
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs )
{
    const StreamLayout& lay = cfg_.layout;
    if ( !gate_.active() )
        return;
    const uint32_t epoch = gate_.epoch();  // see publish()
    const int64_t now_us = steady_now_us();
    if ( warmup_.enter( epoch, now_us ) )
        composer_.reset_canvas();
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
    if ( !( warmup_.ready( now_us ) && pacer_.admit_idle( ts_us, queue_.depth(), epoch ) ) )
    {
        // Held or the encode queue busy: no encode this tick. Multi-view
        // still plates the absent cells that lack it (none after the
        // first such tick: compose_idle skips plated cells).
        if ( lay.nsrc > 1 && !composer_.compose_idle( absent, nabs, ring_.current(), nullptr ) )
            queue_.fail( epoch );
        return;
    }
    ts_us_ = ts_us;
    const int slot = ring_.advance();
    if ( !composer_.compose_idle( absent, nabs, slot, ring_.buf( slot ) ) )
    {
        queue_.fail( epoch );  // see publish()
        return;
    }
    gate_.run( [this]( const SessionGate::Session& s ) {  // see publish()
        if ( !codec_.encode( ring_.frame( ring_.current() ), ts_us_, s, queue_ ) )
            queue_.fail( s.epoch );
    } );
}

