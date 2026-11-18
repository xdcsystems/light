// SourceChannel: one source from attach to decoded frame -- the device
// (MediaSource), its MJPEG decoder, its geometry and its warm-up counter.
//
// State:
//   NeverAttached -- configured, never Live; the decoder is not initialized yet;
//   Detached      -- was Live, the frame thread saw it fail; the decoder stays;
//   Live          -- the frame thread may poll and decode it.
//
// Thread rule (one owner at a time, the state is the hand-over): the attaching
// thread (the Pipeline ctor before the prober starts, then the prober) owns
// every field while state != Live, writes them all and stores Live LAST
// (release); the frame thread loads the state (acquire) and touches the fields
// only while Live; detach() clears the warm-up, then stores Detached (release)
// and stops touching the fields. Outside the rule: last_ok is atomic (stamped
// by attach() and by the frame thread), node() is written by attach() only and
// read only on the attaching thread (busy-node list).
//
// Contracts: the dequeue status ladder lives in source_types.hpp, the Locator
// contract in source/usb_node_locator.hpp.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "config/config.hpp"  // SourceKind, SourceEndpoint
#include "core/geometry.hpp"
#include "core/log.hpp"
#include "core/status.hpp"
#include "jpeg_probe.hpp"
#include "source/source_types.hpp"

template <class Src, class Dec>
class BasicSourceChannel
{
    using clk = std::chrono::steady_clock;

    enum
    {
        kWarmupFrames = 3,  // valid decodes discarded after a hot attach (mid-stream start)
        kAuxRatio = 8,      // over 8x the model input: the two-pass RGA path needs the aux buffer
        kDecErrMax = 4,     // consecutive decoder failures before detaching
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

    explicit BasicSourceChannel( const Log& log ) : log_( log ), src_( log ), dec_( log ) {}
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
        rot_deg_ = ep.rot_deg;
        rot_auto_ = ep.rot_auto;
        model_w_ = model_w;
        model_h_ = model_h;
        
        if ( kind_ == SourceKind::NetMjpeg )
            src_.use_net();
        else
            src_.use_camera();
    }

    void set_cancel( const std::atomic<bool>* stop, const std::atomic<bool>* prober_quit ) noexcept
    {
        src_.set_cancel( stop, prober_quit );
    }

    // The one attach path (startup, hot attach, re-attach), attaching thread only,
    // never while Live. hot: false = the Pipeline ctor (startup log texts, no warm-up).
    // why: the reason when the result is not Live (ok when Live).
    template <class Locator>
    Attach attach( const NodeList& busy, Locator& loc, bool hot, Status& why ) noexcept
    {
        const bool first = state_.load( std::memory_order_acquire ) == State::NeverAttached;
        why = {};
        if ( kind_ == SourceKind::V4L2 )
            why = locate_node( busy, loc, hot, first );
        if ( why.ok() )
            why = src_.open( node_ );
        if ( !why.ok() )
            return Attach::Absent;
        const Attach a = prepare_decode( src_.width(), src_.height(), hot, why );
        if ( a != Attach::Live )
        {
            src_.teardown();
            return a;
        }
        warmup_left_ = hot ? kWarmupFrames : 0;
        dec_err_streak_ = 0;
        exif_rot_.store( -1, std::memory_order_relaxed );
        attach_seq_.fetch_add( 1u, std::memory_order_relaxed );
        last_ok_.store( clk::now() );
        state_.store( State::Live, std::memory_order_release );  // LAST: publishes every field above
        if ( !hot )
            log_.info() << "source: geometry " << geom_.w << 'x' << geom_.h << '\n';
        else
            log_.info() << "source s" << idx_ << ( first ? ": attached (hot) - geometry " : ": re-attached - geometry " )
                        << geom_.w << 'x' << geom_.h << '\n';
        return Attach::Live;
    }

    // Frame thread, after a Detached/Fatal poll: closes the source and hands back to prober.
    void detach()
    {
        warmup_left_ = 0;
        dec_err_streak_ = 0;
        exif_rot_.store( -1, std::memory_order_relaxed );
        src_.teardown();
        state_.store( State::Detached, std::memory_order_release );
    }

    // Frame thread, Live only. wait_ms < 0: the blocking single-source dequeue
    // (the camera's own 2 s watchdog reports EIO).
    FrameStatus poll( int wait_ms, EncodedFrame& out )
    {
        out.src = idx_;
        out.buf = -1;
        out.size = 0;
        out.st = src_.dequeue( out.buf, out.size, wait_ms );
        switch ( out.st.code() )
        {
            case Errc::Ok:
                last_ok_.store( clk::now() );
                return FrameStatus::Frame;
            case Errc::Timeout:
                return FrameStatus::NotReady;
            case Errc::Lost:
                return FrameStatus::Detached;
            default:
                return FrameStatus::Fatal;
        }
    }

    // Frame thread, after a Frame poll: decodes, requeues the buffer and applies
    // the warm-up. Returns the decoded dma-buf fd, or -1 when there is no frame
    // to publish:
    //   empty packet -- a dying/reconnecting camera hands out bytesused == 0; MPP
    //                   "decodes" it into a stale buffer, so it is never decoded;
    //   decode error -- does not consume the warm-up;
    //   warm-up      -- the first valid decodes after a hot attach may be partial
    //                   (mid-stream start): decoded, then discarded.
    int decode( const EncodedFrame& f, Status* err = nullptr ) noexcept
    {
        int fd = -1;
        Status st;
        if ( f.size > 0 )
        {
            const auto* jpg = static_cast<const uint8_t*>( src_.buffer( f.buf ) );
            maybe_probe_exif( jpg, f.size );
            st = dec_.decode( jpg, f.size, fd );
        }
        else
            log_.warn() << "source s" << idx_ << ": zero-size frame skipped\n";
        src_.requeue( f.buf );
        if ( !st.ok() )
        {
            ++dec_err_streak_;
            if ( st.code() == Errc::NoMemory )
            {
                dec_ready_ = false;
                geom_.aux_fd = -1;
            }
            if ( err != nullptr && ( st.code() == Errc::NoMemory || dec_err_streak_ >= kDecErrMax ) )
                *err = Status( Errc::Lost, st.what(), st.kind(), st.sys() );
            return -1;
        }
        if ( fd < 0 )
            return -1;
        dec_err_streak_ = 0;
        if ( warmup_left_ > 0 )
        {
            --warmup_left_;
            if ( warmup_left_ == 0 )
                log_.info() << "source s" << idx_ << ": warm-up done, publishing resumed\n";
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
    uint32_t attach_seq() const noexcept { return attach_seq_.load( std::memory_order_acquire ); }
    int exif_rot() const noexcept { return exif_rot_.load( std::memory_order_relaxed ); }
    bool rot_auto() const noexcept { return rot_auto_; }
    int rot_deg() const noexcept { return rot_deg_; }
    auto out_fmt() const { return dec_.out_fmt(); }
    int hor_stride() const noexcept { return dec_.hor_stride(); }
    int ver_stride() const noexcept { return dec_.ver_stride(); }
    // Attaching thread only.
    const std::string& node() const { return node_; }

    clk::time_point last_ok() const { return last_ok_.load(); }
    void stamp_ok( clk::time_point tp ) { last_ok_.store( tp ); }

private:
    // Camera node lookup with the startup / hot-attach / re-attach log texts.
    template <class Locator>
    Status locate_node( const NodeList& busy, Locator& loc, bool hot, bool first )
    {
        if ( !hot && !loc.exists( node_ ) )
            log_.warn() << "camera: configured node " << node_ << " not present, falling back to USB discovery\n";
        std::string found;
        auto trial = [this]( const std::string& n ) { return src_.trial( n ); };
        if ( Status st = loc.locate( node_, busy, trial, found ); !st.ok() )
            return st;
        if ( found == node_ )
            return {};
        if ( !hot )
            log_.info() << "camera: discovery adopted " << found << '\n';
        else if ( first )
            log_.info() << "camera: hot-attach adopted " << node_ << " -> " << found << '\n';
        else
            log_.info() << "source s" << idx_ << ": identity adopted: " << node_ << " -> " << found << '\n';
        node_ = found;
        return {};
    }

    void maybe_probe_exif( const uint8_t* jpg, uint32_t size ) noexcept
    {
        if ( !rot_auto_ || jpg == nullptr || exif_rot_.load( std::memory_order_relaxed ) != -1 )
            return;
        JpegSofInfo sof {};
        if ( probe_jpeg_sof( jpg, size, sof ) )
            exif_rot_.store( sof.exif_rot_deg >= 0 ? sof.exif_rot_deg : -2, std::memory_order_relaxed );
    }

    bool quarter_turn() const noexcept
    {
        return rot_deg_ == 90 || rot_deg_ == 270;
    }

    bool needs_aux( int w, int h ) const noexcept
    {
        if ( w > kAuxRatio * model_w_ || h > kAuxRatio * model_h_ )
            return true;
        return ( rot_auto_ || quarter_turn() ) && ( h > kAuxRatio * model_w_ || w > kAuxRatio * model_h_ );
    }

    // Decoder (first attach only), geometry guard, aux buffer, letterbox.
    Attach prepare_decode( int w, int h, bool hot, Status& why )
    {
        if ( dec_ready_ && ( w != dec_w_ || h != dec_h_ ) )
        {
            // The decoder's buffers are sized for the first geometry; feeding it
            // another size is not supported yet -- stay absent and say why.
            log_.warn() << "source s" << idx_ << ": geometry changed " << dec_w_ << 'x' << dec_h_ << " -> " << w << 'x' << h
                        << " - decoder stays at " << dec_w_ << 'x' << dec_h_ << ", not attaching\n";
            why = make_status( Errc::Unsupported, "geometry changed" );
            return Attach::DecoderFailed;
        }
        if ( !dec_ready_ )
        {
            if ( Status st = dec_.init( w, h ); !st.ok() )
            {
                if ( hot )
                    log_.warn() << "probe s" << idx_ << ": mpp decoder init failed - staying absent\n";
                why = st;
                return Attach::DecoderFailed;
            }
            dec_ready_ = true;
            dec_w_ = w;
            dec_h_ = h;
        }
        if ( needs_aux( w, h ) && geom_.aux_fd < 0 )
        {
            const int w_aux = ( ( w / 2 ) + 1 ) & ~1;
            const int h_aux = ( ( h / 2 ) + 1 ) & ~1;
            const int w_aux_stride = ( w_aux + 15 ) & ~15;
            const int h_aux_stride = ( h_aux + 15 ) & ~15;
            geom_.aux_fd = dec_.aux_fd( static_cast<size_t>( w_aux_stride ) * static_cast<size_t>( h_aux_stride ) * 2 );
            if ( geom_.aux_fd < 0 )
            {
                if ( hot )
                    log_.warn() << "probe s" << idx_ << ": aux buffer alloc failed - staying absent\n";
                why = make_status( Errc::NoMemory, "aux buffer alloc failed" );
                return Attach::AuxFailed;
            }
        }
        geom_.raw_w = w;
        geom_.raw_h = h;
        geom_.rot_deg = rot_deg_;
        const bool swap = quarter_turn();
        geom_.w = swap ? h : w;
        geom_.h = swap ? w : h;
        geom_.lb = band_geom( geom_.w, geom_.h, model_w_, model_h_ );
        return Attach::Live;
    }

private:
    const Log& log_;
    Src src_;  // declared before dec_: the decoder dies first, as in the old SourceSlot
    Dec dec_;
    std::atomic<State> state_ { State::NeverAttached };
    std::atomic<clk::time_point> last_ok_ {};  // last good dequeue: the stall detector's clock
    std::atomic<uint32_t> attach_seq_ { 0 };
    std::atomic<int> exif_rot_ { -1 };

    SourceKind kind_ = SourceKind::V4L2;
    std::string node_;  // V4L2 node or URL; discovery may adopt another node
    int idx_ = 0;
    int rot_deg_ = 0;
    bool rot_auto_ = true;
    int model_w_ = 0;
    int model_h_ = 0;
    SourceGeom geom_ {};
    int warmup_left_ = 0;
    int dec_err_streak_ = 0;
    bool dec_ready_ = false;  // decoder initialized (replaces the old w = h = 0 rollbacks)
    int dec_w_ = 0;
    int dec_h_ = 0;  // the geometry the decoder was initialized for
};
