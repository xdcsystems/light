#define MODULE_TAG "light_ai"

#include <algorithm>
#include <array>
#include <rockchip/mpp_log.h>

#include "media/mpp_decoder.hpp"
#include "core/log.hpp"
#include "media/dma_sync.hpp"
#include "core/jpeg_probe.hpp"

namespace
{
    constexpr size_t kPktPad = 64;

    bool is_jpeg_pad( uint8_t b ) noexcept
    {
        return b == '\r' || b == '\n' || b == ' ' || b == 0x00;
    }

    bool has_soi_eoi( const uint8_t* data, size_t& size ) noexcept
    {
        while ( size > 2 && is_jpeg_pad( data[size - 1] ) )
            --size;
        if ( size < 4 || data[0] != 0xFF || data[1] != 0xD8 )
            return false;
        if ( data[size - 2] == 0xFF && data[size - 1] == 0xD9 )
            return true;
        // Some UVC cameras append non-zero trailer metadata after EOI; search the last 64 B.
        const size_t scan_start = size > 66 ? size - 64 : 2;
        const uint8_t eoi[2] = { 0xFF, 0xD9 };
        const uint8_t* const found = std::find_end( data + scan_start, data + size, eoi, eoi + 2 );
        if ( found == data + size )
            return false;
        size = static_cast<size_t>( found - data ) + 2;
        return true;
    }

    void strip_port_tasks( MppCtx ctx, const MppApi* mpi, MppPortType port ) noexcept
    {
        enum { kMaxTasks = 16 };
        std::array<MppTask, kMaxTasks> tasks {};
        int n = 0;
        while ( n < kMaxTasks )
        {
            if ( mpi->poll( ctx, port, MPP_POLL_NON_BLOCK ) != MPP_OK )
                break;
            MppTask t = nullptr;
            if ( mpi->dequeue( ctx, port, &t ) != MPP_OK || !t )
                break;
            MppPacket old_p = nullptr;
            MppFrame old_f = nullptr;
            mpp_task_meta_get_packet( t, KEY_INPUT_PACKET, &old_p );
            mpp_task_meta_get_frame( t, KEY_OUTPUT_FRAME, &old_f );
            tasks[static_cast<size_t>( n++ )] = t;
        }
        for ( int i = 0; i < n; ++i )
            mpi->enqueue( ctx, port, tasks[static_cast<size_t>( i )] );
    }
}  // namespace

Status MppDecoder::init( int w, int h )
{
    if ( ctx_ )
        teardown();
    w_ = w;
    h_ = h;
    // MPP logs its version banner, rc/prep config and teardown meta dumps
    // at info/error level by default. Silence it for production runs;
    // raise back to MPP_LOG_DEBUG temporarily when debugging MPP internals.
    // (Replaces the former dynamic-init file static MppLogQuieter: the
    // project rule bans statics with constructors, and the call is
    // idempotent, so per-Decoder invocation is safe.)
    mpp_set_log_level( MPP_LOG_WARN );

    Status st;
    do
    {
        MPP_RET rc = mpp_create( &ctx_, &mpi_ );
        if ( rc != MPP_OK )
        {
            log_.error() << "mpp_create failed\n";
            st = sdk_status( Errc::Failed, "mpp_create failed", rc );
            break;
        }
        rc = mpp_init( ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG );
        if ( rc != MPP_OK )
        {
            log_.error() << "mpp_init failed\n";
            st = sdk_status( Errc::Failed, "mpp_init failed", rc );
            break;
        }

        // Pool for input packets (DRM: the decoder reads the packet directly)
        MPP_RET rg = mpp_buffer_group_get( &pkt_group_, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, MODULE_TAG, __FUNCTION__ );
        if ( rg != MPP_OK )
        {
            log_.error() << "mpp_buffer_group_get pkt failed\n";
            st = sdk_status( Errc::NoMemory, "mpp_buffer_group_get failed", rg );
            break;
        }

        // Pool for output frames (DRM; the buffer size lives in the
        // aligned-height note right below).
        rg = mpp_buffer_group_get( &out_group_, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, MODULE_TAG, __FUNCTION__ );
        if ( rg != MPP_OK )
        {
            log_.error() << "mpp_buffer_group_get failed\n";
            st = sdk_status( Errc::NoMemory, "mpp_buffer_group_get failed", rg );
            break;
        }

        MppBuffer buf = nullptr;
        // Pool for output frames. Camera stream is 4:2:2, so the buffer is x2, not x1.5.
        // MPP requires the frame buffer height aligned to 16 (1920x1080 needs
        // 1088 rows: slot_size 4177920 vs 4147200 for exact 1080). 640x480 was
        // accidentally aligned and hid this; the decoder asserts before it can
        // even report info_change, so the constructor must allocate the safe size.
        const int w_align = ( w_ + 15 ) & ~15;
        const int h_align = ( h_ + 15 ) & ~15;
        rg = mpp_buffer_get( out_group_, &buf, static_cast<size_t>( w_align ) * h_align * 2 );
        if ( rg != MPP_OK )
        {
            log_.error() << "mpp_buffer_get failed\n";
            st = sdk_status( Errc::NoMemory, "mpp_buffer_get failed", rg );
            break;
        }
        mpp_frame_init( &out_frame_ );
        mpp_frame_set_width( out_frame_, w_ );
        mpp_frame_set_height( out_frame_, h_ );
        mpp_frame_set_hor_stride( out_frame_, static_cast<uint32_t>( w_align ) );
        mpp_frame_set_ver_stride( out_frame_, static_cast<uint32_t>( h_align ) );
        mpp_frame_set_fmt( out_frame_, MPP_FMT_YUV422SP );
        mpp_frame_set_buffer( out_frame_, buf );
        mpp_buffer_put( buf );
        hor_stride_ = w_align;
        ver_stride_ = h_align;
        ok_ = true;
    }
    while ( false );

    if ( !st.ok() )
    {
        teardown();
        return st;
    }
    return {};
}

MppDecoder::~MppDecoder()
{
    teardown();
}

void MppDecoder::teardown() noexcept
{
    if ( !ctx_ )
        return;

    // Strip the meta references from the tasks left in the ports. The decoder
    // recycles consumed input tasks back to the user side of the input port
    // with the packet pointer still in the meta. Once we deinit that packet,
    // the pointer dangles, and the decoder's dec_release_task_in_port (run by
    // reset() and by mpp_destroy()) dereferences it and deinits it a second
    // time ("negative ref_count" / "invalid mem pool ptr"). reset_and_strip()
    // strips BEFORE reset() as well as after it, because reset() itself calls
    // dec_release_task_in_port. The stripped tasks are re-enqueued: the same
    // helper serves the runtime Timeout / Failed recovery of
    // take_output_task(), where dropping them would shrink the port queues
    // for good; here it makes no difference, mpp_destroy() frees the tasks
    // with their queues.
    // Only after a complete init (ok_): a failed init step has queued no
    // task, and after a failed mpp_init the ctx is not initialized.
    if ( ok_ )
        reset_and_strip();

    // Critical order: release every MPP object that may hold buffer
    // references (pkt_, pkt_buf_, out_frame_, aux_) BEFORE mpp_destroy().
    // Destroy tears down MPP's internal buffer pools, and a later
    // mpp_packet_deinit / mpp_frame_deinit / mpp_buffer_put would decrement
    // refcounts in an already freed pool ("non-positive ref_count" +
    // "invalid mem pool ptr" at exit). The buffer groups go after destroy.
    if ( pkt_ )
    {
        mpp_packet_deinit( &pkt_ );
        pkt_ = nullptr;
    }
    if ( pkt_buf_ )
    {
        mpp_buffer_put( pkt_buf_ );
        pkt_buf_ = nullptr;
        pkt_buf_cap_ = 0;
    }
    if ( out_frame_ )
    {
        mpp_frame_deinit( &out_frame_ );
        out_frame_ = nullptr;
    }

    if ( aux_ )
    {
        mpp_buffer_put( aux_ );
        aux_ = nullptr;
    }

    mpp_destroy( ctx_ );
    ctx_ = nullptr;
    mpi_ = nullptr;

    if ( out_group_ )
    {
        mpp_buffer_group_put( out_group_ );
        out_group_ = nullptr;
    }
    if ( pkt_group_ )
    {
        mpp_buffer_group_put( pkt_group_ );
        pkt_group_ = nullptr;
    }
    w_ = 0;
    h_ = 0;
    hor_stride_ = 0;
    ver_stride_ = 0;
    out_fmt_ = MPP_FMT_YUV422SP;
    pts_ = 0;
    frames_ = 0;
    ok_ = false;
}

Status MppDecoder::decode( const uint8_t* data, size_t size, int& fd ) noexcept
{
    fd = -1;
    ++frames_;
    if ( Status st = make_packet( data, size ); !st.ok() || !pkt_ )
        return st;
    if ( Status st = submit_input(); !st.ok() )
        return st;
    return retrieve_output( fd );
}

// Phase 1/3: validate the input and build the input packet. Returns Ok
// (pkt_ fully initialized, not yet referenced by the decoder) or Ok with
// pkt_ == nullptr (no frame) for a bad input / a mid-stream fragment without
// SOI/EOI (warn logged); NoMemory on the packet buffer allocation failure,
// Failed when an MPP buffer or packet call fails.
Status MppDecoder::make_packet( const uint8_t* data, size_t size )
{
    // Defensive deinit, before the early returns: Ok with pkt_ == nullptr is
    // how decode() learns that this input built no packet. On every normal
    // return path the packet is already deinit'ed - its lifetime is tied to
    // the output dequeue (see the notes at each return). This only catches
    // paths where the decoder provably holds no reference (no enqueue has
    // happened).
    drop_packet();

    if ( !ok_ || !data || size == 0 )
    {
        log_.warn() << "dec[" << frames_ << "]: BAD INPUT ok=" << ok_ << " data=" << static_cast<const void*>( data ) << " size=" << size
                    << '\n';
        return {};
    }

    // Mid-stream fragment guard: a complete camera MJPEG frame always
    // starts with SOI (FFD8). Packets torn out of the middle of the
    // stream (right after STREAMON, before the first full frame lands,
    // or from a dying camera) can be decoded by MPP "successfully" —
    // no errinfo, no discard — into a partial garbage image that then
    // reaches RGA/NPU. Reject anything without SOI.
    if ( !has_soi_eoi( data, size ) )
    {
        log_.warn() << "dec[" << frames_ << "]: missing SOI/EOI, truncated MJPEG fragment (" << size << " bytes) dropped\n";
        return {};
    }

    // Chroma-layout detection from the JPEG SOF marker, now via the shared
    // structural probe (jpeg_probe.hpp). The MPP MJPEG decoder follows the
    // stream subsampling (4:2:0 -> 420SP, 4:2:2 -> 422SP) and ignores the
    // fmt pre-set on the user frame (the green-bottom A/B); the RGA read
    // side must match. The probe walks markers up to SOS — a stricter check
    // than the old 2 KiB scan; any parsing doubt keeps the previous value.
    {
        JpegSofInfo info {};
        if ( probe_jpeg_sof( data, size, info ) )
        {
            const MppFrameFormat want = info.yuv420 ? MPP_FMT_YUV420SP : MPP_FMT_YUV422SP;
            if ( want != out_fmt_ )
            {
                out_fmt_ = want;
                if ( out_frame_ )
                    mpp_frame_set_fmt( out_frame_, out_fmt_ );
                log_.info() << "dec[" << frames_ << "]: jpeg chroma 4:2:" << ( want == MPP_FMT_YUV420SP ? "0" : "2" )
                            << " -> out fmt " << ( out_fmt_ == MPP_FMT_YUV420SP ? "420SP" : "422SP" ) << '\n';
            }
        }
    }

    // Safe to overwrite pkt_buf_ unconditionally: decode() is synchronous and every
    // Timeout/Failed path calls reset_and_strip() + drop_packet(), so MPP holds no ref here.
    if ( Status bst = ensure_pkt_buf( size + kPktPad ); !bst.ok() )
        return bst;
    void* ptr = mpp_buffer_get_ptr( pkt_buf_ );
    if ( !ptr )
    {
        log_.error() << "dec: mpp_buffer_get_ptr returned null (" << size << ")\n";
        mpp_buffer_put( pkt_buf_ );
        pkt_buf_ = nullptr;
        pkt_buf_cap_ = 0;
        return make_status( Errc::Failed, "mpp_buffer_get_ptr failed" );
    }
    {
        const dma::DmaSync wr( mpp_buffer_get_fd( pkt_buf_ ), true );
        auto* dst = static_cast<uint8_t*>( ptr );
        std::copy_n( data, size, dst );
        std::fill_n( dst + size, kPktPad, uint8_t { 0 } );
        dma::cache_clean( dst, size + kPktPad );
    }
    return wrap_pkt_buf( size );
}

Status MppDecoder::wrap_pkt_buf( size_t size )
{
    // pkt_ holds its own ref while pkt_buf_ stays cached.
    if ( const MPP_RET rp = mpp_packet_init_with_buffer( &pkt_, pkt_buf_ ); rp != MPP_OK || !pkt_ )
    {
        log_.error() << "dec: mpp_packet_init_with_buffer failed (" << rp << ")\n";
        drop_packet();
        return sdk_status( Errc::Failed, "mpp_packet_init_with_buffer failed", rp );
    }
    mpp_packet_set_size( pkt_, size );
    mpp_packet_set_length( pkt_, size );
    mpp_packet_set_pts( pkt_, ++pts_ );
    return {};
}

Status MppDecoder::ensure_pkt_buf( size_t size )
{
    if ( pkt_buf_ && size <= pkt_buf_cap_ )
        return {};
    if ( pkt_buf_ )
    {
        mpp_buffer_put( pkt_buf_ );
        pkt_buf_ = nullptr;
        pkt_buf_cap_ = 0;
    }
    const MPP_RET rg = mpp_buffer_get( pkt_group_, &pkt_buf_, size );
    if ( rg != MPP_OK || !pkt_buf_ )
    {
        log_.error() << "dec: pkt buffer alloc failed (" << size << ")\n";
        pkt_buf_ = nullptr;
        return sdk_status( Errc::NoMemory, "mpp_buffer_get failed", rg );
    }
    pkt_buf_cap_ = size;
    return {};
}

// Phase 2/3: hand the packet to the decoder. On entry the packet is
// owned by us; on success it is enqueued and the decoder holds it in
// the task meta. On any failure no enqueue happened, so the packet is
// dropped here - the decoder provably holds no reference.
Status MppDecoder::submit_input()
{
    // Input: wait for a free task (bounded: the input queue is a few
    // tasks deep, a free task must appear within a frame interval)
    if ( mpi_->poll( ctx_, MPP_PORT_INPUT, static_cast<MppPollType>( kTaskPollMs ) ) != MPP_OK )
    {
        log_.warn() << "dec: no free input task in " << kTaskPollMs << " ms\n";
        // No input task was taken, the decoder holds no reference to the
        // packet: deinit here, not on the next call.
        drop_packet();
        return make_status( Errc::Timeout, "no free input task" );
    }
    MppTask task = nullptr;
    const MPP_RET rc = mpi_->dequeue( ctx_, MPP_PORT_INPUT, &task );
    if ( rc != MPP_OK || !task )
    {
        log_.warn() << "dec: input dequeue failed\n";
        // No task was taken, the packet is not referenced: deinit here.
        drop_packet();
        return sdk_status( Errc::Failed, "input dequeue failed", rc );
    }
    mpp_task_meta_set_packet( task, KEY_INPUT_PACKET, pkt_ );
    mpp_task_meta_set_frame( task, KEY_OUTPUT_FRAME, out_frame_ );

    const MPP_RET ret = mpi_->enqueue( ctx_, MPP_PORT_INPUT, task );

    if ( ret != MPP_OK )
    {
        log_.warn() << "dec: input enqueue ret=" << ret << '\n';
        // Hand the task back so the decoder thread is not stuck waiting
        // for it forever on the next poll. A failed re-enqueue means the
        // queue state is broken: log it loudly, no third attempt.
        // Packet is dropped with the failed task: pop both meta keys via
        // get_* (set_*(nullptr) inserts a null entry and leaves meta_size > 0).
        MppPacket old_p = nullptr;
        MppFrame old_f = nullptr;
        mpp_task_meta_get_packet( task, KEY_INPUT_PACKET, &old_p );
        mpp_task_meta_get_frame( task, KEY_OUTPUT_FRAME, &old_f );
        if ( mpi_->enqueue( ctx_, MPP_PORT_INPUT, task ) != MPP_OK )
        {
            log_.error() << "dec: task re-enqueue failed, decoder queue state broken\n";
        }
        // The failed enqueue means the decoder never took the task, the
        // packet is not referenced: safe to deinit now.
        drop_packet();
        return sdk_status( Errc::Failed, "input enqueue failed", ret );
    }
    return {};
}

// Phase 3/3: wait for the decoded frame and hand it back. On a Timeout /
// Failed take_output_task the input task (holding pkt_ in its meta) is
// still queued inside the decoder: reset() flushes all queued tasks and
// releases their meta references, only after it is drop_packet() safe.
Status MppDecoder::retrieve_output( int& fd )
{
    MppTask task = nullptr;
    if ( Status st = take_output_task( task ); !st.ok() )
        return st;

    MppFrame f = nullptr;
    mpp_task_meta_get_frame( task, KEY_OUTPUT_FRAME, &f );
    if ( f && mpp_frame_get_info_change( f ) )
        return apply_info_change( f, task );
    if ( const Status st = frame_fd( f, fd ); !st.ok() )
        return st;
    if ( fd >= 0 )
        update_strides( f );

    mpi_->enqueue( ctx_, MPP_PORT_OUTPUT, task );
    // The output task is back in the free list, the decoder no longer
    // references the packet: safe point for deinit (instead of the top
    // of the next call, which raced with the still-enqueued task on the
    // Timeout / Failed paths of the previous frame).
    drop_packet();
    return {};
}

// The output task of retrieve_output(): Ok with the task set, or a
// Timeout / Failed Status with the decoder flushed and the packet dropped.
Status MppDecoder::take_output_task( MppTask& task )
{
    // Output: wait for the decoded frame (bounded: steady decode is
    // ~2-3 ms; a timeout means MPP is wedged — surface it instead of
    // hanging the pipeline thread forever)
    if ( mpi_->poll( ctx_, MPP_PORT_OUTPUT, static_cast<MppPollType>( kTaskPollMs ) ) != MPP_OK )
    {
        // The input task (holding pkt_ in its meta) is still queued inside
        // the decoder. reset_and_strip() pops any residual task meta and
        // flushes the ports before drop_packet().
        log_.warn() << "dec: output not ready in " << kTaskPollMs << " ms - flushing decoder\n";
        reset_and_strip();
        drop_packet();
        return make_status( Errc::Timeout, "output not ready" );
    }
    const MPP_RET rc = mpi_->dequeue( ctx_, MPP_PORT_OUTPUT, &task );
    if ( rc != MPP_OK || !task )
    {
        // See the Timeout path: flush and strip task meta before drop_packet().
        log_.warn() << "dec: output dequeue failed - flushing decoder\n";
        reset_and_strip();
        drop_packet();
        return sdk_status( Errc::Failed, "output dequeue failed", rc );
    }
    return {};
}

// The info_change frame f (from the meta of task): the output frame is
// re-sized in place with a new buffer, the task returned, the packet dropped.
// Returns Ok -- the info_change frame itself is skipped (normal) -- or
// NoMemory on the buffer allocation failure.
void MppDecoder::update_strides( MppFrame f ) noexcept
{
    if ( !f )
        return;
    const int hs = static_cast<int>( mpp_frame_get_hor_stride( f ) );
    const int vs = static_cast<int>( mpp_frame_get_ver_stride( f ) );
    if ( hs > 0 )
        hor_stride_ = hs;
    if ( vs > 0 )
        ver_stride_ = vs;
    if ( out_frame_ && hs > 0 && vs > 0 )
    {
        mpp_frame_set_hor_stride( out_frame_, static_cast<uint32_t>( hs ) );
        mpp_frame_set_ver_stride( out_frame_, static_cast<uint32_t>( vs ) );
    }
}

Status MppDecoder::apply_info_change( MppFrame f, MppTask task )
{
    const uint32_t w = mpp_frame_get_width( f );
    const uint32_t h = mpp_frame_get_height( f );
    const uint32_t hs = mpp_frame_get_hor_stride( f );
    const uint32_t vs = mpp_frame_get_ver_stride( f );
    const size_t bs = mpp_frame_get_buf_size( f );
    log_.warn() << "dec: info_change " << w << 'x' << h << " stride=" << hs << 'x' << vs << " buf_size=" << bs << '\n';

    // Return the output task BEFORE touching out_frame_ (get_frame in
    // retrieve_output() already popped KEY_OUTPUT_FRAME, leaving meta_size == 0).
    mpi_->enqueue( ctx_, MPP_PORT_OUTPUT, task );

    // Allocate the replacement buffer BEFORE mutating out_frame_ or signaling READY.
    MppBuffer b = nullptr;
    const MPP_RET rg = mpp_buffer_get( out_group_, &b, bs );
    if ( rg != MPP_OK || !b )
    {
        log_.error() << "dec: alloc " << bs << " bytes FAILED\n";
        // No INFO_CHANGE_READY follows: the same recovery as the output Timeout / Failed paths.
        reset_and_strip();
        drop_packet();
        return sdk_status( Errc::NoMemory, "mpp_buffer_get failed", rg );
    }

    mpp_frame_set_width( out_frame_, w );
    mpp_frame_set_height( out_frame_, h );
    if ( hs > 0 )
    {
        mpp_frame_set_hor_stride( out_frame_, hs );
        hor_stride_ = static_cast<int>( hs );
    }
    if ( vs > 0 )
    {
        mpp_frame_set_ver_stride( out_frame_, vs );
        ver_stride_ = static_cast<int>( vs );
    }
    // Follow the SOF subsampling already parsed into out_fmt_.
    mpp_frame_set_fmt( out_frame_, out_fmt_ );
    // In-place re-set: the task meta we just returned still holds this
    // same frame object, so it must not be destroyed and re-created.
    mpp_frame_set_buffer( out_frame_, b );
    mpp_buffer_put( b );

    mpi_->control( ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr );
    // The info_change task has been returned: the packet is consumed.
    drop_packet();
    return {};  // the info_change frame itself is skipped — this is normal
}

// The dma-buf fd of a decoded (not info_change) frame into fd; Ok with
// fd = -1 when the meta holds no frame, the frame carries errinfo /
// discard, or it has no buffer (each case warn-logged).
Status MppDecoder::frame_fd( MppFrame f, int& fd ) const
{
    fd = -1;
    if ( !f )
    {
        log_.warn() << "dec: output task WITHOUT frame in meta\n";
        return {};
    }
    if ( mpp_frame_get_errinfo( f ) || mpp_frame_get_discard( f ) )
    {
        log_.warn() << "dec: DECODE-ERROR errinfo=0x" << std::hex << mpp_frame_get_errinfo( f ) << std::dec
                    << " discard=" << mpp_frame_get_discard( f ) << " fmt=" << static_cast<int>( mpp_frame_get_fmt( f ) ) << '\n';
        return {};
    }
    MppBuffer buf = mpp_frame_get_buffer( f );
    if ( !buf )
    {
        log_.warn() << "dec: frame WITHOUT buffer\n";
        return {};
    }
    fd = mpp_buffer_get_fd( buf );
    return {};
}

void MppDecoder::reset_and_strip()
{
    // Pop any residual KEY_INPUT_PACKET / KEY_OUTPUT_FRAME nodes from all
    // tasks sitting in both ports before AND after mpi_->reset() reconciles
    // the queues.
    for ( const MppPortType port : { MPP_PORT_INPUT, MPP_PORT_OUTPUT } )
        strip_port_tasks( ctx_, mpi_, port );
    mpi_->reset( ctx_ );
    for ( const MppPortType port : { MPP_PORT_INPUT, MPP_PORT_OUTPUT } )
        strip_port_tasks( ctx_, mpi_, port );
}

int MppDecoder::aux_fd( size_t bytes )
{
    if ( !ok_ )
        return -1;
    if ( aux_ )
    {
        // The fd contract implies "at least `bytes` usable": a second
        // call with a larger size silently returning the old, smaller
        // buffer would end in an out-of-bounds RGA write later. Compare
        // against the real allocation, reallocate on mismatch.
        const size_t have = mpp_buffer_get_size( aux_ );
        if ( have >= bytes )
            return mpp_buffer_get_fd( aux_ );
        log_.warn() << "dec: aux buffer too small (" << have << " < " << bytes << "), reallocating\n";
        mpp_buffer_put( aux_ );
        aux_ = nullptr;
    }
    if ( mpp_buffer_get( out_group_, &aux_, bytes ) != MPP_OK )
        return -1;
    return mpp_buffer_get_fd( aux_ );
}

