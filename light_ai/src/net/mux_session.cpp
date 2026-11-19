#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
}

#include "av_common.hpp"  // av_err_str
#include "core/fixed_fmt.hpp"
#include "core/log.hpp"
#include "muxer_teardown.hpp"
#include "net/mux_session.hpp"
#include "tcp_pull_server.hpp"

MuxSession::MuxSession( const EncoderConfig& cfg, PacketQueue& queue, SessionGate& gate, const Log& log )
  : log_( log )
  , cfg_( cfg )
  , queue_( queue )
  , gate_( gate )
  , params_( log )
{
}

MuxSession::~MuxSession()
{
    stop();
}

void MuxSession::start()
{
    thread_ = std::thread( &MuxSession::serve, this );
}

void MuxSession::stop()
{
    quit_.store( true );
    queue_.stop();  // wakes a drain blocked in pop()
    {
        const std::lock_guard<std::mutex> lk( tcp_mx_ );
        if ( active_tcp_ != nullptr )
            active_tcp_->abort_client();
    }
    if ( thread_.joinable() )
        thread_.join();
}

void MuxSession::serve() noexcept
{
    // Empty host = pull mode: listen on TCP, the viewer connects on its own.
    const bool tcp_pull = cfg_.host.empty();
    const std::string url = tcp_pull ? std::string() 
                                     : ( "udp://" + cfg_.host + ':' + std::to_string( cfg_.port ) + "?pkt_size=1316" );

    TcpPullServer tcp( cfg_.port, 256 * 1024, 5 );
    struct TcpReg
    {
        std::mutex& mx;
        TcpPullServer*& slot;
        TcpReg( std::mutex& m, TcpPullServer*& s, TcpPullServer& t ) : mx( m ), slot( s )
        {
            const std::lock_guard<std::mutex> lk( mx );
            slot = &t;
        }
        ~TcpReg()
        {
            const std::lock_guard<std::mutex> lk( mx );
            slot = nullptr;
        }
    } tcp_reg( tcp_mx_, active_tcp_, tcp );
    if ( tcp_pull )
    {
        // The bind retries until it succeeds or stop() ends the thread;
        // the run keeps going without a viewer (README, Run).
        if ( Status st = retry_listen( [&] { return tcp.listen(); }, quit_, log_, "enc", cfg_.port ); !st.ok() )
            return;
        log_.info() << "enc: PULL mode: listening on tcp port " << cfg_.port << 
                     " (viewer: ffplay -fflags nobuffer tcp://<board-ip>:" << cfg_.port << ")\n";
    }

    while ( !quit_.load() )
    {
        // Session body extracted (tidy: readability-function-size).
        // continue;/break; became return;: each failure path now falls
        // through to serve()'s trailing print + UDP backoff, same as before.
        run_session( tcp, tcp_pull, url );
        if ( !quit_.load() )
        {
            log_.info() << ( tcp_pull ? "enc: viewer gone, waiting for the next one" 
                                    : "enc: udp path reset, reopening in 500 ms" ) << "\n";

            if ( !tcp_pull )
                std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        }
    }
}

// Batch C follow-up: the mux side of serve(), split out for tidy
// (readability-function-size). drain_queue() owns the wait/pop loop;
// mux_write() writes one packet with the late-join IDR-prepend logic
// (ParamSetCache; the Annex B scans are annexb:: in core/param_set_cache.hpp).
MuxSession::DrainEnd MuxSession::drain_queue( AVFormatContext* fc, const TcpPullServer* tcp )
{
    MuxPerfWindow mperf {};
    auto win_t0 = std::chrono::steady_clock::now();
    uint64_t prev_ww = tcp != nullptr ? tcp->wait_writable_cnt() : 0;
    // The queue's key gate was armed by open() (late join mid-GOP must
    // not decode P-frames against a missing reference) and is re-armed by
    // an overflow flush; pop() drops packets until the first IDR.
    for ( ;; )
    {
        const auto pop_t0 = std::chrono::steady_clock::now();
        const PacketQueue::Popped got = queue_.pop( mux_pkt_.slot() );
        const double pop_ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - pop_t0 ).count();
        if ( pop_ms > mperf.pop_wait_ms_max )
            mperf.pop_wait_ms_max = pop_ms;
        if ( got.what == PacketQueue::Pop::Stopped || got.what == PacketQueue::Pop::Failed )
        {
            maybe_log_perf( mperf, win_t0, prev_ww, tcp, true );
            return got.what == PacketQueue::Pop::Stopped ? DrainEnd::Stopped : DrainEnd::Failed;
        }
        const int q_rem = got.left;
        // Ownership of the packet buffer moved into mux_pkt_ via
        // av_packet_move_ref; mux_write consumes and unrefs it without
        // freeing the preallocated AVPacket struct.
        const auto mw_t0 = std::chrono::steady_clock::now();
        const int r = mux_write( fc, mux_pkt_.get() );
        const double mw_ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - mw_t0 ).count();
        mperf.add_mux_write( mw_ms );
        ++mperf.pkts;
        if ( mw_ms > 20.0 )
            log_.warn() << "enc: mux_write stalled " << static_cast<long>( mw_ms ) << " ms (queue was " << q_rem << ")\n";
        maybe_log_perf( mperf, win_t0, prev_ww, tcp, false );
        if ( r < 0 )
        {
            maybe_log_perf( mperf, win_t0, prev_ww, tcp, true );
            if ( r == AVERROR( EPIPE ) )
                log_.info() << "enc: viewer disconnected (tcp), session ends\n";
            else if ( r == AVERROR( ETIMEDOUT ) )
                log_.info() << "enc: viewer stalled (send timeout), session ends\n";
            else
                log_.error() << "enc: write failed: " << av_err_str( r ) << " (packet lost)\n";
            return DrainEnd::WriteFailed;
        }
    }
}

void MuxSession::maybe_log_perf( MuxPerfWindow& mperf,
                                 std::chrono::steady_clock::time_point& win_t0,
                                 uint64_t& prev_ww,
                                 const TcpPullServer* tcp,
                                 bool force ) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double, std::milli>( now - win_t0 ).count();
    if ( ( !force && elapsed < 5000.0 ) || mperf.pkts == 0 )
        return;
    const uint64_t ww_now = tcp != nullptr ? tcp->wait_writable_cnt() : 0;
    mperf.wait_writable_cnt = static_cast<uint32_t>( ww_now - prev_ww );
    prev_ww = ww_now;
    mperf.pkt_q_max_depth = static_cast<uint32_t>( queue_.take_max_depth() );
    log_.info() << "[perf] mux pkts=" << mperf.pkts
                << " pop_max=" << Fixed { mperf.pop_wait_ms_max, 1 }
                << " mw_hist=[" << mperf.mux_write_hist[0] << ',' << mperf.mux_write_hist[1] << ','
                << mperf.mux_write_hist[2] << ',' << mperf.mux_write_hist[3]
                << "] ww=" << mperf.wait_writable_cnt
                << " pq_max=" << mperf.pkt_q_max_depth << '\n';
    mperf.reset();
    win_t0 = now;
}

int MuxSession::mux_write( AVFormatContext* fc, AVPacket* pkt )
{
    // Late-join support: ParamSetCache captures the SPS/PPS prefix of the
    // first IDR and prepends it to every later IDR that does not carry
    // parameter sets of its own (core/param_set_cache.hpp); the prepended
    // copy goes out through dup_pkt_. The TS muxer tolerates the duplicate.
    AVPacket* dup = dup_pkt_.get();
    AVPacket* out = pkt;
    const ByteSpan b = params_.prepare( pkt->data, static_cast<size_t>( pkt->size ) );
    if ( b.data != pkt->data )
    {
        av_packet_copy_props( dup, pkt );
        dup->buf = nullptr;
        dup->data = b.data;
        dup->size = static_cast<int>( b.size );
        out = dup;
    }
    const int r = av_write_frame( fc, out );
    dup->data = nullptr;
    dup->size = 0;
    av_packet_unref( dup );
    av_packet_unref( pkt );
    return r;
}

// Transport teardown: one point, shared by the destructor and the
// session lifetimes. Verbatim from the former close_transport lambda
// in run_session(); members replace the lambda captures.
MuxSession::Transport::~Transport()
{
    close();
}

void MuxSession::Transport::close()
{
    if ( tcp_pull )
    {
        if ( tcp )
            tcp->close_client();
    }
    else if ( udp_pb )
    {
        avio_closep( &udp_pb );
    }
}

// Out-of-line dtor of the class-local FcGuard: 
// nulls pb first (the Transport destructor owns the avio context),
// then frees the muxer context.
MuxSession::FcGuard::~FcGuard()
{
    teardown_muxer( p );  // muxer_teardown.hpp: pb is detached first (the
                          // Transport destructor owns the avio context)
}

// Transport setup, verbatim move out of run_session(): TCP pull
// (poll the listener, accept, socket options, custom AVIO over the
// fd) or UDP push (avio_open, no handshake). The sleep on the UDP
// failure path stays here, exactly where it was in run_session().
Status MuxSession::open_transport( const std::string& url, AVFormatContext* fc, Transport& t )
{
    if ( t.tcp_pull )
    {
        if ( !t.tcp )
            return make_status( Errc::Invalid, "no transport" );
        if ( Status ast = t.tcp->accept_client( quit_ ); !ast.ok() )
            return ast;

        log_.info() << "enc: viewer connected (tcp)\n";  // its first frame: an IDR (H264Encoder::encode)

        return t.tcp->attach_avio( fc );
    }

    // UDP: no handshake and no listener — the socket is opened once,
    // packets are pushed regardless of whether a viewer is listening.
    if ( const int ret = avio_open( &t.udp_pb, url.c_str(), AVIO_FLAG_WRITE ); ret < 0 )
    {
        log_.warn() << "enc: udp open failed: " << av_err_str( ret ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return sdk_status( Errc::Failed, "udp open failed", ret );
    }
    fc->pb = t.udp_pb;
    return {};
}

// Stream setup, verbatim move out of run_session(): create the
// stream, fill the codec parameters, write the header with the
// live-stream mpegts options, derive the pts step and print the
// session log lines. Status: NoMemory (avformat_new_stream), Failed
// (write_header; the log and the pause stay here, before the return).
Status MuxSession::setup_stream( AVFormatContext* fc, AVStream** st_out, int64_t* frame_step, bool tcp_pull )
{
    const StreamLayout& lay = cfg_.layout;
    AVStream* st = avformat_new_stream( fc, nullptr );
    if ( !st )
        return make_status( Errc::NoMemory, "avformat_new_stream failed" );
    // codecpar is allocated by avformat_new_stream itself
    //(libavformat/options.c: st->codecpar = 
    // avcodec_parameters_alloc(), with its own fail path):
    // the former manual fallback here was dead code.
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_H264;
    st->codecpar->format = AV_PIX_FMT_NV12;
    st->codecpar->width = lay.w;
    st->codecpar->height = lay.h;
    st->codecpar->bit_rate = cfg_.bit_rate();  // the codec context belongs to the pipeline thread
    st->time_base = cfg_.vfr() ? AVRational { 1, 90000 } : AVRational { 1, cfg_.fps };
    st->avg_frame_rate = cfg_.vfr() ? AVRational { 0, 1 } : AVRational { cfg_.fps, 1 };

    // Live-stream patch: repeat PAT/PMT/SDT in the stream so a receiver
    // can join at any point, not only at stream startup.
    AVDictionary* mux_opts = nullptr;
    av_dict_set( &mux_opts, "mpegts_flags", "+pat_pmt_at_frames+resend_headers", 0 );
    av_dict_set( &mux_opts, "pat_period", "0.1", 0 );
    av_dict_set( &mux_opts, "sdt_period", "0.25", 0 );
    int ret2 = avformat_write_header( fc, &mux_opts );
    av_dict_free( &mux_opts );
    if ( ret2 < 0 )
    {
        log_.error() << "enc: write_header failed: " << av_err_str( ret2 ) << '\n';
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return sdk_status( Errc::Failed, "write_header failed", ret2 );
    }
    const AVRational tb = st->time_base;
    int64_t step = cfg_.vfr() ? 1 : av_rescale_q( 1, AVRational { 1, cfg_.fps }, tb );
    if ( step < 1 )
        step = 1;
    if ( cfg_.vfr() )
        log_.info() << "enc: stream time_base " << tb.num << '/' << tb.den << " (VFR: pts from dequeue times)\n";
    else
        log_.info() << "enc: stream time_base " << tb.num << '/' << tb.den << ", pts step " << step << "\n";
    if ( tcp_pull )
    {
        log_.info() << "enc: tcp pull streaming " << lay.w << 'x' << lay.h << " h264 (mpegts) on port " << cfg_.port << '\n';
    }
    else
    {
        log_.info() << "enc: udp streaming " << lay.w << 'x' << lay.h << " h264 (mpegts) -> " << cfg_.host << ':' << cfg_.port << '\n';
    }
    *st_out = st;
    *frame_step = step;
    return {};
}

// One viewer-session iteration: allocate the muxer context, open the
// transport (TCP pull or UDP push), set up the stream and drain the
// encoder packet queue into the muxer. Context and transport teardown
// is RAII (FcGuard + Transport destructor): every early
// return below releases exactly what the former close_transport();
// avformat_free_context() paths released, in the same order.
void MuxSession::run_session( TcpPullServer& tcp, bool tcp_pull, const std::string& url )
{
    AVFormatContext* raw = nullptr;
    if ( avformat_alloc_output_context2( &raw, nullptr, "mpegts", tcp_pull ? nullptr : url.c_str() ) < 0 || !raw )
    {
        log_.error() << "enc: avformat_alloc_output_context2 failed\n";
        std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
        return;
    }
    // Live stream: the muxer must not hold packets for batching.
    // Defaults (max_delay 0.7 s, preload 0.5 s, buffered file I/O)
    // make the TS writer emit data in bursts - on Wi-Fi that reads as
    // periodic client-side buffering. Flush every packet instead.
    raw->max_delay = 0;
    raw->flush_packets = 1;
    
    // Local guard instead of unique_ptr: the Transport pattern used
    // above; same declaration position -> same destruction order
    // (fc dies AFTER the Transport, as before).
    FcGuard fc { raw };

    Transport t;
    t.tcp = &tcp;
    t.tcp_pull = tcp_pull;
    if ( Status tst = open_transport( url, fc.p, t ); !tst.ok() )
        return;

    AVStream* st = nullptr;
    int64_t frame_step = 1;
    if ( Status sst = setup_stream( fc.p, &st, &frame_step, tcp_pull ); !sst.ok() )
        return;

    // Session open order: the queue first (cleared, key gate armed, this
    // epoch's fail() accepted), then the gate (the encoder may encode into
    // the session from here on). frame_step and the stream index travel
    // in the session; every session's first frame is an IDR (H264Encoder).
    const uint32_t epoch = ++sessions_;
    queue_.open( epoch );
    SessionGate::Session session;
    session.stream_index = st->index;
    session.frame_step = frame_step;
    session.epoch = epoch;
    gate_.open( session );

    // Batch C: drain the encoder packet queue into the muxer. The
    // pipeline thread never blocks on the network from here on: a slow
    // viewer stalls this loop, not publish(). The wait/pop loop and the
    // per-packet write with the IDR-prepend logic moved to drain_queue()
    // and mux_write() (tidy: readability-function-size).
    const DrainEnd end = drain_queue( fc.p, tcp_pull ? &tcp : nullptr );

    // Close order: the gate first (waits for an encode in progress, so
    // nothing is pushed after it), then the queue (the leftover packets
    // are dead weight), then the trailer.
    gate_.close();
    queue_.close();
    // Best effort on a healthy socket: only when the session ended by quit,
    // as before (not after an encode-side failure or a failed write).
    if ( end == DrainEnd::Stopped )
        av_write_trailer( fc.p );
}
