// SourceChannel: one source from attach to decoded frame -- the device
// (MediaSource), its MJPEG decoder, its geometry, its warm-up counter and an
// explicit State. Stage 3a: replaces Pipeline::SourceSlot, init_one_source,
// start_source, init_aux and the body of probe_source (one attach path).
//
// State:
//   NeverAttached -- configured, never Live; the decoder is not initialized yet;
//   Detached      -- was Live, the frame thread saw it fail; the decoder stays;
//   Live          -- the frame thread may poll and decode it.
//
// Thread rule (one owner at a time, the state is the hand-over):
//   attaching thread (the Pipeline ctor before the prober starts, then the prober):
//     owns every field while state != Live; attach() writes them all and
//     stores Live LAST (release);
//   frame thread:
//     loads the state (acquire) and touches the fields only while Live;
//     detach() clears the warm-up, then stores Detached (release) and stops
//     touching the fields;
//   outside the rule:
//     last_ok -- atomic: attach() stamps it, the frame thread stamps and reads it;
//     node()  -- written only by attach(), read only on the attaching thread
//                (busy-node list); the frame thread never reads it.
//
// Template parameters (static polymorphism; tests/test_source_channel.cpp
// instantiates the channel with fakes, src/sources.cpp with the real types):
//   Src:
//     explicit Src( const Log& );
//     void use_camera(); void use_net();                    // kind, once, at configure()
//     int open( const std::string& id, std::string* why );  // 0 = streaming; teardown first
//     int trial( const std::string& node, std::string* why );  // camera: setup + teardown, 0 = ok
//     int dequeue( uint32_t* size, int* err, int timeout_ms );   // buffer index or -1 + errno
//     void requeue( int idx ); void* buffer( int idx ) const;
//     int width() const; int height() const; void teardown();
//   Dec: default-constructible (production);
//     int init( int w, int h );                    // 0 = ok; self-cleans on failure
//     int decode( const uint8_t* data, size_t n );  // dma-buf fd, < 0 = no frame
//     int aux_fd( size_t bytes );                   // fd or -1; lives as long as the decoder
//     out_fmt() const;                              // any type: handed to the RGA read side
//   Locator (attach() argument): see source/usb_node_locator.hpp:
//     int locate( const std::string& preferred, const NodeList& busy, Trial&&, std::string& out );
//     bool exists( const std::string& node ) const;

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.hpp"  // SourceKind, SourceEndpoint
#include "core/geometry.hpp"
#include "core/log.hpp"
#include "source/source_types.hpp"

template <class Src, class Dec>
class BasicSourceChannel
{
    using clk = std::chrono::steady_clock;

    enum
    {
        kWarmupFrames = 3,  // valid decodes discarded after a hot attach (mid-stream start)
        kAuxRatio = 8,      // over 8x the model input: the two-pass RGA path needs the aux buffer
    };

public:
    enum class State : uint8_t
    {
        NeverAttached,
        Detached,
        Live
    };
    // attach() outcome; anything but Live leaves the state unchanged.
    enum class Attach : uint8_t
    {
        Live,
        Absent,         // device not there / not streaming: the prober retries
        DecoderFailed,  // decoder init failed, or the geometry differs from the decoder's
        AuxFailed       // two-pass aux buffer allocation failed
    };

    explicit BasicSourceChannel( const Log& log ) : log_( log ), src_( log ) {}
    // Tests: hands each fake its control block (Src( log, sa ), Dec( da )).
    template <class SrcArg, class DecArg>
    BasicSourceChannel( const Log& log, SrcArg& sa, DecArg& da ) : log_( log ), src_( log, sa ), dec_( da )
    {
    }
    ~BasicSourceChannel() = default;
    BasicSourceChannel( const BasicSourceChannel& ) = delete;
    BasicSourceChannel& operator=( const BasicSourceChannel& ) = delete;
    BasicSourceChannel( BasicSourceChannel&& ) = delete;
    BasicSourceChannel& operator=( BasicSourceChannel&& ) = delete;

    // Pipeline ctor, once, before any attach.
    void configure( int idx, const SourceEndpoint& ep, int model_w, int model_h )
    {
        idx_ = idx;
        kind_ = ep.kind;
        node_ = ep.uri;
        model_w_ = model_w;
        model_h_ = model_h;
        if ( kind_ == SourceKind::NetMjpeg )
            src_.use_net();
        else
            src_.use_camera();
    }

    // The one attach path (startup, hot attach, re-attach), attaching thread only,
    // never while Live. hot: false = the Pipeline ctor (startup log texts, no warm-up).
    // why: the reason when the result is not Live.
    template <class Locator>
    Attach attach( const NodeList& busy, Locator& loc, bool hot, std::string& why )
    {
        const bool first = state_.load( std::memory_order_acquire ) == State::NeverAttached;
        why.clear();
        if ( kind_ == SourceKind::V4L2 && locate_node( busy, loc, hot, first, why ) != 0 )
            return Attach::Absent;
        if ( src_.open( node_, &why ) != 0 )
            return Attach::Absent;
        const Attach a = prepare_decode( src_.width(), src_.height(), hot, why );
        if ( a != Attach::Live )
        {
            src_.teardown();
            return a;
        }
        warmup_left_ = hot ? kWarmupFrames : 0;
        last_ok_.store( clk::now() );
        state_.store( State::Live, std::memory_order_release );  // LAST: publishes every field above
        if ( !hot )
            log_.info() << "source: geometry " << geom_.w << 'x' << geom_.h << '\n';
        else
            log_.info() << "source s" << idx_ << ( first ? ": attached (hot) - geometry " : ": re-attached - geometry " )
                        << geom_.w << 'x' << geom_.h << '\n';
        return Attach::Live;
    }

    // Frame thread, after a Detached/Fatal poll: hands the channel back to the prober.
    void detach()
    {
        warmup_left_ = 0;
        state_.store( State::Detached, std::memory_order_release );
    }

    // Frame thread, Live only. wait_ms < 0: the blocking single-source dequeue
    // (the camera's own 2 s watchdog reports EIO).
    FrameStatus poll( int wait_ms, EncodedFrame& out )
    {
        out.src = idx_;
        out.size = 0;
        out.err = 0;
        out.buf = src_.dequeue( &out.size, &out.err, wait_ms );
        if ( out.buf >= 0 )
        {
            last_ok_.store( clk::now() );
            return FrameStatus::Frame;
        }
        if ( out.err == EAGAIN || out.err == EINTR )
            return FrameStatus::NotReady;
        return ( out.err == ENODEV || out.err == EIO ) ? FrameStatus::Detached : FrameStatus::Fatal;
    }

    // Frame thread, after a Frame poll: decodes, requeues the buffer and applies
    // the warm-up. Returns the decoded dma-buf fd, or -1 when there is no frame
    // to publish:
    //   empty packet -- a dying/reconnecting camera hands out bytesused == 0; MPP
    //                   "decodes" it into a stale buffer, so it is never decoded;
    //   decode error -- does not consume the warm-up;
    //   warm-up      -- the first valid decodes after a hot attach may be partial
    //                   (mid-stream start): decoded, then discarded.
    int decode( const EncodedFrame& f )
    {
        int fd = -1;
        if ( f.size > 0 )
            fd = dec_.decode( static_cast<const uint8_t*>( src_.buffer( f.buf ) ), f.size );
        else
            log_.warn() << "camera: zero-size frame skipped\n";
        src_.requeue( f.buf );
        if ( fd < 0 )
            return -1;
        if ( warmup_left_ > 0 )
        {
            --warmup_left_;
            if ( warmup_left_ == 0 )
                log_.info() << "camera: warm-up done, publishing resumed\n";
            return -1;
        }
        return fd;
    }

    State state() const { return state_.load( std::memory_order_acquire ); }
    bool live() const { return state() == State::Live; }
    SourceKind kind() const { return kind_; }
    int index() const { return idx_; }
    // Frame thread while Live (see the thread rule).
    const SourceGeom& geom() const { return geom_; }
    auto out_fmt() const { return dec_.out_fmt(); }
    // Attaching thread only.
    const std::string& node() const { return node_; }

    clk::time_point last_ok() const { return last_ok_.load(); }
    void stamp_ok( clk::time_point tp ) { last_ok_.store( tp ); }

private:
    // Camera node lookup with the startup / hot-attach / re-attach log texts.
    template <class Locator>
    int locate_node( const NodeList& busy, Locator& loc, bool hot, bool first, std::string& why )
    {
        if ( !hot && !loc.exists( node_ ) )
            log_.warn() << "camera: configured node " << node_ << " not present, falling back to USB discovery\n";
        std::string found;
        auto trial = [this]( const std::string& n, std::string* w ) { return src_.trial( n, w ); };
        if ( loc.locate( node_, busy, trial, found ) != 0 )
        {
            why = "no usable USB capture node found";
            return -1;
        }
        if ( found == node_ )
            return 0;
        if ( !hot )
            log_.info() << "camera: discovery adopted " << found << '\n';
        else if ( first )
            log_.info() << "camera: hot-attach adopted " << node_ << " -> " << found << '\n';
        else
            log_.info() << "source s" << idx_ << ": identity adopted: " << node_ << " -> " << found << '\n';
        node_ = found;
        return 0;
    }

    // Decoder (first attach only), geometry guard, aux buffer, letterbox.
    Attach prepare_decode( int w, int h, bool hot, std::string& why )
    {
        if ( dec_ready_ && ( w != dec_w_ || h != dec_h_ ) )
        {
            // The decoder's buffers are sized for the first geometry; feeding it
            // another size is not supported yet -- stay absent and say why.
            log_.warn() << "source s" << idx_ << ": geometry changed " << dec_w_ << 'x' << dec_h_ << " -> " << w << 'x' << h
                        << " - decoder stays at " << dec_w_ << 'x' << dec_h_ << ", not attaching\n";
            why = "geometry changed";
            return Attach::DecoderFailed;
        }
        if ( !dec_ready_ )
        {
            if ( dec_.init( w, h ) != 0 )
            {
                if ( hot )
                    log_.warn() << "probe s" << idx_ << ": mpp decoder init failed - staying absent\n";
                why = "mpp decoder init failed";
                return Attach::DecoderFailed;
            }
            dec_ready_ = true;
            dec_w_ = w;
            dec_h_ = h;
        }
        if ( ( w > kAuxRatio * model_w_ || h > kAuxRatio * model_h_ ) && geom_.aux_fd < 0 )
        {
            geom_.aux_fd = dec_.aux_fd( static_cast<size_t>( w / 2 ) * static_cast<size_t>( h / 2 ) * 2 );
            if ( geom_.aux_fd < 0 )
            {
                if ( hot )
                    log_.warn() << "probe s" << idx_ << ": aux buffer alloc failed - staying absent\n";
                why = "aux buffer alloc failed";
                return Attach::AuxFailed;
            }
        }
        geom_.w = w;
        geom_.h = h;
        geom_.lb = band_geom( w, h, model_w_, model_h_ );
        return Attach::Live;
    }

private:
    const Log& log_;
    Src src_;  // declared before dec_: the decoder dies first, as in the old SourceSlot
    Dec dec_ {};
    std::atomic<State> state_ { State::NeverAttached };
    std::atomic<clk::time_point> last_ok_ {};  // last good dequeue: the stall detector's clock

    SourceKind kind_ = SourceKind::V4L2;
    std::string node_;  // V4L2 node or URL; discovery may adopt another node
    int idx_ = 0;
    int model_w_ = 0;
    int model_h_ = 0;
    SourceGeom geom_ {};
    int warmup_left_ = 0;
    bool dec_ready_ = false;  // decoder initialized (replaces the old w = h = 0 rollbacks)
    int dec_w_ = 0;
    int dec_h_ = 0;  // the geometry the decoder was initialized for
};
