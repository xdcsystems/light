#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/multipart_reader.hpp"
#include "core/status.hpp"
#include "source/buffered_socket.hpp"
#include "source/mjpeg_slot_pool.hpp"

class Log;

// HTTP MJPEG pull source (IP Webcam / any multipart/x-mixed-replace server).
// Media stays JPEG end to end: the MPP decoder consumes these
// packets exactly like the USB camera's MJPEG buffers -- no transcode anywhere.
// The only cost vs V4L2 is one memcpy per frame (socket -> slot);
// V4L2 hands the dma-buf zero-copy, HTTP cannot.
//
// The TCP client and its receive window (source/buffered_socket.hpp), the response
// head and the multipart framing as pure parsers over that window
// (core/http_head.hpp, core/multipart_reader.hpp), the frame slots (source/mjpeg_slot_pool.hpp).
// This class keeps the source contract of MediaSource: connect + prime,
// the start() drain, the latest-frame skip, the frame check
// (jpeg_frame_check), the corrupt-frame streak and the dequeue ladder (dequeue_status).
//
// Thread rule (the Src of SourceChannel, source/source_channel.hpp: one
// owner at a time, the channel state is the hand-over):
//   call                                         thread                        when
//   ctor                                         the Pipeline ctor             SourceChannel::configure()
//   setup_device + start (MediaSource::open),    the attaching thread: the     channel state != Live
//     width / height, teardown after a failed      Pipeline ctor, then
//     decoder attach                               SourceProber
//   dequeue, requeue, buffer                     the frame thread              channel Live
//   dtor                                         the Pipeline dtor             the prober joined
// No field is shared: SourceChannel::attach() stores Live last (release),
// detach() stores Detached (release), both sides load it acquire. The class
// has no lock and no atomic of its own. The ctor throws (a failed frame-pool
// mmap): it runs only from configure() on the startup path -- a throw on the
// prober thread would terminate the process. detach() does not close the
// socket: it stays open (unread) until the next open() on the attaching
// thread tears it down.
//
// Slot 0 reuse in dequeue(): SourceChannel::decode() decodes synchronously
// (MppDecoder::decode copies the JPEG into its own MPP buffer) and requeues
// right after, on the frame thread, so at most one slot is in flight and
// "all buffers in flight" is unreachable; if a caller ever broke the
// contract, no decode could be reading slot 0 at that point -- the only
// reader is the frame thread itself, inside dequeue().

class NetMjpegSource
{
    enum
    {
        kPollMs = 2000,     // per-fill watchdog, same window as the camera
        kErrStreakMax = 4,  // corrupt/short-frame streak tolerance
        kSkipMax = 4,       // read_latest_part: parts skipped per dequeue at most
        kDrainMax = 64,     // start(): parts drained at most
    };
    // One part read (read_part, read_latest_part).
    enum class Read : uint8_t
    {
        Part,   // one part in the slot
        Lost,   // the watchdog, EOF or a socket error: the connection is gone
        Desync  // the stream does not parse, or the window wedged full: drop it, resync
    };
    // The next part (poll_part_ready).
    enum class Ready : uint8_t
    {
        Yes,    // complete in the window (or the window wedged full: the read desyncs)
        No,     // not within the wait (an EINTR included)
        Lost,   // EOF or a socket error
        Desync  // the window wedged full while waiting
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
    Status setup_device( const std::string& url );
    void teardown();
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
    Status dequeue( int& index, uint32_t& size, int timeout_ms = -1 );
    void requeue( int index );
    void* buffer( int index ) const { return pool_.data( index ); }
    size_t buffer_size( int ) const { return pool_.capacity(); }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    Status send_get( const std::string& host, int port, const std::string& path ) const;
    // Response head (core/http_head.hpp) -> the reader's boundary.
    Status read_headers();
    // After the head: the first JPEG frame primed into slot 0, its SOFn
    // geometry checked against a locked one (a reconnect).
    Status prime_first_frame();
    // One complete part (or a wedged-full window) is buffered: no fill needed.
    bool part_buffered() const;
    // The desync protocol: the buffered window is dropped, the reader resyncs.
    void drop_window();
    // Fills (waiting up to timeout_ms, then without waiting) until a part is complete.
    Ready poll_part_ready( int timeout_ms );
    // dequeue's wait: ok = a part is buffered; else EAGAIN, or EIO once the streak is full.
    Status wait_part_ready( int timeout_ms );
    // dequeue's first frame after setup_device / start: the primed one in
    // slot 0 (primed_len_ > 0), handed out once.
    Status take_primed( int& index, uint32_t& size );
    // One part into slot, then the freshest of the parts already buffered
    // (via a scratch slot, at most kSkipMax); a Desync gets one resync retry.
    Read read_latest_part( int slot, uint32_t* size );
    // One part into slot (core/multipart_reader.hpp), refilling with the
    // kPollMs watchdog. Any result but Part resyncs the reader (the next read
    // starts at a delimiter line); only the callers drop the window on Desync.
    Read read_part( int slot, uint32_t* size );

private:
    const Log& log_;
    MjpegSlotPool pool_;  // first: a failed mmap throws before anything else exists
    BufferedSocket sock_;
    MultipartReader parts_ { static_cast<size_t>( MjpegSlotPool::kSlotBytes ) };  // the boundary is set per connect
    int err_streak_ = 0;
    int width_ = 0, height_ = 0;
    uint32_t primed_len_ = 0;  // first frame read during setup_device to extract SOF dims
};
