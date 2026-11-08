#pragma once
#include <cstddef>
#include <cstdint>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

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
    // w/h come from Config (validated against the camera G_FMT) instead of
    // being hard-coded: the pre-allocated output buffer must match the
    // actual stream size, otherwise a resolution change either triggers
    // info_change churn or overruns the buffer.
    MppDecoder( int w, int h );
    ~MppDecoder();

    int decode( const uint8_t* data, size_t size );

    // Phase boundaries of decode(): the input packet lifetime is owned
    // by exactly one phase at a time --
    // validation + packet creation, hand-off to the decoder,
    // retrieval of the decoded frame.
    // The negative return codes of decode() come from the failing phase.
    int make_packet( const uint8_t* data, size_t size );
    int submit_input();
    int retrieve_output();
    // Extra dma-buf from the same DRM pool: intermediate buffer for
    // multi-pass RGA when the scale ratio exceeds the RGA3 limit (1/8).
    int aux_fd( size_t bytes );  // returns fd or -1; buffer lives until dtor

    // Actual decoder output layout, detected per-frame from the JPEG SOF
    // marker (see make_packet):
    // 420SP for 4:2:0 streams, 422SP for 4:2:2/4:4:4.
    // The RGA read side must match;
    // before the first frame the value is the legacy camera default (422SP).
    MppFrameFormat out_fmt() const { return out_fmt_; }

private:
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
    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;
    MppBufferGroup out_group_ = nullptr;
    MppFrame out_frame_ = nullptr;
    MppPacket pkt_ = nullptr;
    MppBufferGroup pkt_group_ = nullptr;
    MppBuffer aux_ = nullptr;

    int w_ = 0;
    int h_ = 0;
    MppFrameFormat out_fmt_ = MPP_FMT_YUV422SP;  // SOF-detected, see make_packet
    uint64_t pts_ = 0;
    int frames_ = 0;
    bool ok_ = false;
};
