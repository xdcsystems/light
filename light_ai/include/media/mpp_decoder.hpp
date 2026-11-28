#pragma once
#include <cstddef>
#include <cstdint>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

#include "core/status.hpp"

class Log;

class MppDecoder
{
    // Bounded MPP polls: a wedged decoder must not hang the pipeline
    // thread forever -- camera recovery cannot help a thread that is
    // stuck inside decode() rather than in DQBUF.
    // Steady decode is ~2-3 ms; 1 s is a huge margin even for the worst case.
    enum : int
    {
        kTaskPollMs = 1000
    };

public:
    // w/h come from the active source (camera G_FMT or network JPEG SOF):
    // the pre-allocated output buffer matches the actual stream size.
    // Diagnostics go to log: the chroma switch to its `out` stream, the rest to `err`.
    explicit MppDecoder( const Log& log ) : log_( log ) {}
    ~MppDecoder();
    MppDecoder( const MppDecoder& ) = delete;
    MppDecoder& operator=( const MppDecoder& ) = delete;

    // One-shot setup for the source geometry: Ok, or the failing SDK step
    // (Failed / NoMemory); the failing call is logged inside.
    Status init( int w, int h );

    // Decode one packet. Ok with fd >= 0: the dma-buf of a decoded frame.
    // Ok with fd = -1: this call has no publishable frame (bad input, no
    // SOI, an errinfo / discard frame) -- normal on the recovery paths.
    // A failure Status: broken decoder state (Timeout: a poll wait,
    // NoMemory: a buffer, Failed: an SDK step); fd is -1 then.
    Status decode( const uint8_t* data, size_t size, int& fd ) noexcept;

    // Extra dma-buf from the same DRM pool: intermediate buffer for
    // multi-pass RGA when the scale ratio exceeds the RGA3 limit (1/8).
    int aux_fd( size_t bytes );  // returns fd or -1; buffer lives until dtor

    // Actual decoder output layout, detected per-frame from the JPEG SOF
    // marker (see make_packet):
    // 420SP for 4:2:0 streams, 422SP for 4:2:2/4:4:4.
    // The RGA read side must match;
    // before the first frame the value is the legacy camera default (422SP).
    MppFrameFormat out_fmt() const { return out_fmt_; }
    int hor_stride() const noexcept { return hor_stride_; }
    int ver_stride() const noexcept { return ver_stride_; }

private:
    // Phase boundaries of decode(): the input packet lifetime is owned
    // by exactly one phase at a time --
    // validation + packet creation, hand-off to the decoder,
    // retrieval of the decoded frame.
    // A phase returns Ok with fd = -1 when the call simply has no frame
    // (bad input, no SOI, an errinfo / discard frame), and a failure
    // Status when the decoder state is broken (Timeout / Failed / NoMemory).
    Status make_packet( const uint8_t* data, size_t size );
    Status submit_input();
    Status retrieve_output( int& fd );
    void reset_and_strip();
    // The steps of retrieve_output(), on the decode() caller's thread like the
    // whole class: the output task (Timeout on the poll, Failed on the
    // dequeue), the info_change frame (Ok: skipped, NoMemory on the buffer),
    // the fd of a decoded frame (Ok with fd = -1: no frame, errinfo / discard,
    // no buffer).
    Status take_output_task( MppTask& task );
    Status apply_info_change( MppFrame f, MppTask task );
    Status frame_fd( MppFrame f, int& fd ) const;

    // Allocates/grows the persistent pkt_buf_ to at least `size` bytes.
    Status ensure_pkt_buf( size_t size );

    // The one release path (dtor, re-init, a failed init() step): releases
    // what exists and resets the decoder state; a no-op without a ctx_.
    void teardown() noexcept;
    void update_strides( MppFrame f ) noexcept;

    // Packet teardown mechanics. Deinit is legal only at points where
    // the decoder provably holds no reference --
    // each call site in decode() carries the reasoning in its comment.
    // The null-check here lets every path use the same call.
    void drop_packet()
    {
        if ( pkt_ )
        {
            mpp_packet_deinit( &pkt_ );
            pkt_ = nullptr;
        }
    }

private:
    const Log& log_;
    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppBufferGroup out_group_ = nullptr;
    MppFrame out_frame_ = nullptr;
    MppPacket pkt_ = nullptr;
    MppBuffer pkt_buf_ = nullptr;
    size_t pkt_buf_cap_ = 0;
    MppBufferGroup pkt_group_ = nullptr;
    MppBuffer aux_ = nullptr;

    int w_ = 0;
    int h_ = 0;
    int hor_stride_ = 0;
    int ver_stride_ = 0;
    MppFrameFormat out_fmt_ = MPP_FMT_YUV422SP;  // SOF-detected, see make_packet
    uint64_t pts_ = 0;
    int frames_ = 0;
    bool ok_ = false;
};
