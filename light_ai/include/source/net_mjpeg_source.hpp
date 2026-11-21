#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/multipart_reader.hpp"
#include "core/status.hpp"
#include "source/buffered_socket.hpp"
#include "source/mjpeg_slot_pool.hpp"

class Log;

// HTTP MJPEG pull source (any multipart/x-mixed-replace server, e.g. IP Webcam).
// Frames stay JPEG end-to-end: the MPP decoder consumes them like the USB
// camera's MJPEG buffers -- no transcode. Only cost vs V4L2 is one memcpy
// per frame (socket -> slot); V4L2 gives zero-copy via dma-buf.
//
// Built from independent parts: the TCP client + receive window
// (source/buffered_socket.hpp), the HTTP head and multipart framing as pure
// parsers (core/http_head.hpp, core/multipart_reader.hpp), the frame slots
// (source/mjpeg_slot_pool.hpp). This class keeps the MediaSource contract:
// connect + prime, the start() drain, latest-frame skip, jpeg_frame_check,
// corrupt-frame streak, dequeue ladder (dequeue_status).
//
// Threading (SourceChannel, source/source_channel.hpp: single owner, channel
// state is the hand-over):
//   ctor            -> Pipeline ctor thread      (SourceChannel::configure())
//   setup_device, start/open, teardown -> the attaching thread (Pipeline ctor
//                       or SourceProber), channel state != Live
//   dequeue/requeue/buffer -> frame thread       (channel Live)
//   dtor            -> Pipeline dtor             (prober joined)
// No shared fields: SourceChannel::attach() stores Live last (release),
// detach() stores Detached (release); both sides load acquire. No locks or
// atomics of its own. The ctor throws (failed frame-pool mmap) only from
// configure() on the startup path -- a throw on the prober thread would kill
// the process. detach() leaves the socket open (unread) until the next
// open() on the attaching thread tears it down.
//
// Slot 0 reuse: SourceChannel::decode() runs synchronously on the frame
// thread (MppDecoder::decode copies the JPEG into its own MPP buffer) and
// requeues immediately, so at most one slot is in flight and "all buffers
// in flight" is unreachable. Even if a caller broke the contract, the only
// slot-0 reader is the frame thread itself inside dequeue().

class NetMjpegSource
{
    enum
    {
        kPollMs = 2000,     // per-fill watchdog, same window as the camera; also the whole-head deadline
        kPartMs = 5000,     // one blocking part, from its first header byte: a 300 KB JPEG needs >= 0.48 Mbit/s
        kErrStreakMax = 4,  // corrupt/short-frame streak tolerance
        kSkipMax = 4,       // read_latest_part: parts skipped per dequeue at most
        kDrainMax = 64,     // start(): parts drained at most
    };
    // One part read (read_part, step_nonblocking_part, read_latest_part).
    enum class Read : uint8_t
    {
        Part,       // one part in the slot
        Pending,    // non-blocking poll timed out or interrupted mid-part; rx_slot_ stays active
        Lost,       // the watchdog, EOF or a socket error: the connection is gone
        Desync,     // the window wedged full of non-delimited bytes: drop it, resync
        Oversized   // a > kSlotBytes part was drained to the next delimiter: keep the window
    };

public:
    // Throws when the frame pool cannot be mapped (MjpegSlotPool): the
    // startup path only (MediaSource::use_net <- SourceChannel::configure).
    explicit NetMjpegSource( const Log& log ) : log_( log ) {}
    ~NetMjpegSource() = default;  // the socket closes, the pool unmaps
    NetMjpegSource( const NetMjpegSource& ) = delete;
    NetMjpegSource& operator=( const NetMjpegSource& ) = delete;
    NetMjpegSource( NetMjpegSource&& ) = delete;
    NetMjpegSource& operator=( NetMjpegSource&& ) = delete;

    // Full connect path for an http://host[:port]/path URL. Non-throwing: not
    // ok tears down and says why (the url, connect, send, head and first-frame
    // reasons; Absent = nobody there, so the prober retries).
    void set_cancel( const std::atomic<bool>* stop, const std::atomic<bool>* prober_quit = nullptr ) noexcept
    {
        sock_.set_cancel( stop, prober_quit );
    }
    Status setup_device( const std::string& url );
    void teardown() noexcept;
    // Drains frames accumulated in the TCP socket during Pipeline init
    // so run() starts at the live edge alongside CameraV4L2::start().
    void start();

    // ok: index = the buffer holding one complete JPEG frame, size = its length.
    // Otherwise dequeue_status( errno ), the same ladder as the camera:
    // EIO = recoverable stall (watchdog, hangup, corrupt-frame streak, desync),
    // ENODEV = closed by teardown, EAGAIN = no frame ready within timeout_ms.
    // Corrupt/incomplete frames are dropped with EAGAIN (or EIO once the
    // streak reaches kErrStreakMax); the streak survives across calls and
    // resets only on a clean frame or a successful setup_device().
    // timeout_ms < 0: the blocking single-source mode (the 2 s watchdog).
    Status dequeue( int& index, uint32_t& size, int timeout_ms = -1 ) noexcept;
    void requeue( int index ) noexcept;
    void* buffer( int index ) const noexcept { return pool_.data( index ); }
    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t skip_parts_cnt() const noexcept { return skip_parts_cnt_; }

private:
    Status send_get( const std::string& host, int port, const std::string& path ) const;
    // Response head (core/http_head.hpp) -> the reader's boundary.
    Status read_headers();
    // After the head: the first JPEG frame primed into slot 0, its SOFn
    // geometry adopted (also on a reconnect at another size).
    Status prime_first_frame();
    // One complete part is buffered in the window: no fill needed.
    bool part_buffered() const;
    // Pulls the bytes already queued in the kernel socket buffer (0 ms polls) until a whole part is buffered
    // or nothing more is queued.
    void pull_queued();
    // The desync protocol: the buffered window is dropped, the reader resyncs.
    void drop_window();
    int acquire_rx_slot();
    // Streams multipart bytes into slot without exceeding timeout_ms (timeout_ms >= 0).
    Read step_nonblocking_part( int slot, int timeout_ms, uint32_t* size );
    // dequeue's first frame after setup_device / start: the primed one in
    // slot 0 (primed_len_ > 0), handed out once.
    Status take_primed( int& index, uint32_t& size );
    void skip_buffered_parts( int slot, uint32_t& n );
    // A Desync / Oversized result counts one err_streak_ step (a Desync also
    // drops the window); true for those, false for any other result.
    bool note_bad_part( Read rc );
    // One part into slot (blocking if timeout_ms < 0, non-blocking otherwise),
    // then the freshest already buffered part; a Desync / Oversized gets one
    // retry, and every bad part (the retry too) counts toward kErrStreakMax.
    Read read_latest_part( int slot, uint32_t* size, int timeout_ms );
    // One part into slot (core/multipart_reader.hpp), refilling with the
    // kPollMs watchdog; once the part's headers have started, under the kPartMs
    // wall-clock deadline (blocking single-source mode and setup only: the
    // non-blocking multi-source path is bounded by the SourceSet stall timer).
    // Any result but
    // Part resyncs the reader (the next read starts at a delimiter line); only
    // the callers drop the window on Desync.
    Read read_part( int slot, uint32_t* size );
    // fill_until( kPollMs ) cut to what is left before deadline: a server that
    // drips a byte per window no longer resets the watchdog forever (M-15).
    BufferedSocket::Fill fill_before( std::chrono::steady_clock::time_point deadline );
    // The wait policies of feed_part, defined in net_mjpeg_source.cpp (plain
    // structs, no virtuals): how the next bytes are waited for, when a part
    // deadline is armed, and which wait result leaves the part pending.
    struct PollWait;      // step_nonblocking_part
    struct PartDeadline;  // read_part
    // The one feed / consume loop of a part into slot: Part, Oversized (a
    // Desync of the reader: resynced), Pending (wait.pending() of the last
    // fill: the part and rx_slot_ are kept), else resynced: Desync on a full
    // window, Lost otherwise.
    template <typename Wait>
    Read feed_part( int slot, uint32_t* size, Wait& wait );

private:
    const Log& log_;
    MjpegSlotPool pool_;  // first: a failed mmap throws before anything else exists
    BufferedSocket sock_;
    MultipartReader parts_ { static_cast<size_t>( MjpegSlotPool::kSlotBytes ) };  // the boundary is set per connect
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    uint32_t primed_len_ = 0;  // first frame read during setup_device to extract SOF dims
    int rx_slot_ = -1;         // in-progress slot for non-blocking multi-call JPEG reads
    uint64_t skip_parts_cnt_ = 0;
};
