#include <cstdint>

#include "core/encode_pacer.hpp"
#include "core/encoder_config.hpp"
#include "core/log.hpp"
#include "net_encoder.h"

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
    // Multi-view: frames closer than the pacing step only update their
    // tile in the canvas (core/encode_pacer.hpp).
    const bool do_enc = pacer_.admit( ts_us, queue_.depth(), epoch );
    if ( do_enc )
    {
        ts_us_ = ts_us;
        ring_.advance();
    }

    // Fence wait + RGA crop run OUTSIDE the lock: the critical section
    // below now covers only encode + mux. In multi-view mode when two
    // cameras finish within a few ms of each other, !do_enc still updates
    // the source's tile in the persistent composite canvas (out = nullptr)
    // without emitting a duplicate 2-ms-apart H.264 frame.
    const int slot = ring_.current();
    if ( !composer_.compose( tensor_fd, band, src_idx, slot, do_enc ? ring_.buf( slot ) : nullptr ) )
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
// The frame counter and the timing stats never see idle frames.
void NetEncoder::publish_idle( int64_t ts_us, const bool* absent, int nabs )
{
    const StreamLayout& lay = cfg_.layout;
    if ( !gate_.active() )
        return;
    const uint32_t epoch = gate_.epoch();  // see publish()
    bool any = false;
    for ( int s = 0; s < lay.nsrc; ++s )
        any = any || ( !absent || s >= nabs || absent[ s ] );
    if ( !any )
        return;  // every tile went live since the tick was scheduled
    if ( !pacer_.admit_idle( ts_us, queue_.depth(), epoch ) )
        return;  // encode queue busy: skip this tick
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

