#define MODULE_TAG "light_ai"

#include <cstring>
#include <iostream>

#include "mpp_decoder.hpp"

MppDecoder::MppDecoder( int w, int h ) : w_( w ), h_( h )
{
    do
    {
        if ( mpp_create( &ctx_, &mpi_ ) != MPP_OK )
        {
            std::cerr << "mpp_create failed\n";
            break;
        }
        if ( mpp_init( ctx_, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG ) != MPP_OK )
        {
            std::cerr << "mpp_init failed\n";
            break;
        }

        // Pool for input packets (DRM: the decoder reads the packet directly)
        if ( mpp_buffer_group_get( &pkt_group_, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, MODULE_TAG, __FUNCTION__ ) != MPP_OK )
        {
            std::cerr << "mpp_buffer_group_get pkt failed\n";
            break;
        }

        // Pool for output frames. Camera stream is 4:2:2, so the buffer is x2, not x1.5
        if ( mpp_buffer_group_get( &out_group_, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, MODULE_TAG, __FUNCTION__ ) != MPP_OK )
        {
            std::cerr << "mpp_buffer_group_get failed\n";
            break;
        }

        MppBuffer buf = nullptr;
        // Pool for output frames. Camera stream is 4:2:2, so the buffer is x2, not x1.5.
        // MPP requires the frame buffer height aligned to 16 (1920x1080 needs
        // 1088 rows: slot_size 4177920 vs 4147200 for exact 1080). 640x480 was
        // accidentally aligned and hid this; the decoder asserts before it can
        // even report info_change, so the constructor must allocate the safe size.
        const int h_align = ( h_ + 15 ) & ~15;
        if ( mpp_buffer_get( out_group_, &buf, static_cast<size_t>( w_ ) * h_align * 2 ) != MPP_OK )
        {
            std::cerr << "mpp_buffer_get failed\n";
            break;
        }
        mpp_frame_init( &out_frame_ );
        mpp_frame_set_width( out_frame_, w_ );
        mpp_frame_set_height( out_frame_, h_ );
        mpp_frame_set_fmt( out_frame_, MPP_FMT_YUV422SP );
        mpp_frame_set_buffer( out_frame_, buf );
        mpp_buffer_put( buf );
        ok_ = true;
    }
    while ( 0 );

    if ( !ok_ )
    {
        if ( out_frame_ )
        {
            mpp_frame_deinit( &out_frame_ );
            out_frame_ = nullptr;
        }
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
        if ( ctx_ )
        {
            mpp_destroy( ctx_ );
            ctx_ = nullptr;
            mpi_ = nullptr;
        }
    }
}

MppDecoder::~MppDecoder()
{
    if ( !ctx_ )
        return;
    mpi_->reset( ctx_ );
    mpp_destroy( ctx_ );
    ctx_ = nullptr;
    mpi_ = nullptr;
    // Critical order: the packet holds a reference to a buffer from pkt_group_,
    // so it must be released BEFORE its buffer group. The reversed order
    // leaks one packet-sized drm buffer (~33 KB).
    if ( pkt_ )
    {
        mpp_packet_deinit( &pkt_ );
        pkt_ = nullptr;
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
}

int MppDecoder::decode( const uint8_t* data, size_t size )
{
    if ( !ok_ || !data || size == 0 )
    {
        std::cerr << "dec[" << frames_ << "]: BAD INPUT ok=" << ok_ << " data=" << static_cast<const void*>( data ) << " size=" << size
                  << '\n';
        ++frames_;
        return -1;
    }
    ++frames_;

    // Mid-stream fragment guard: a complete camera MJPEG frame always
    // starts with SOI (FFD8). Packets torn out of the middle of the
    // stream (right after STREAMON, before the first full frame lands,
    // or from a dying camera) can be decoded by MPP "successfully" —
    // no errinfo, no discard — into a partial garbage image that then
    // reaches RGA/NPU. Reject anything without SOI.
    if ( size < 2 || data[0] != 0xFF || data[1] != 0xD8 )
    {
        std::cerr << "dec[" << frames_ << "]: no SOI, mid-stream fragment (" << size << " bytes) dropped\n";
        return -1;
    }

    if ( pkt_ )
    {
        mpp_packet_deinit( &pkt_ );
        pkt_ = nullptr;
    }

    MppBuffer pbuf = nullptr;
    if ( mpp_buffer_get( pkt_group_, &pbuf, size ) != MPP_OK )
    {
        std::cerr << "dec: pkt buffer alloc failed (" << size << ")\n";
        return -2;
    }
    memcpy( mpp_buffer_get_ptr( pbuf ), data, size );
    mpp_packet_init_with_buffer( &pkt_, pbuf );  // the packet holds its own reference
    mpp_buffer_put( pbuf );                      // release ours
    mpp_packet_set_pts( pkt_, ++pts_ );

    // Input: wait for a free task
    if ( mpi_->poll( ctx_, MPP_PORT_INPUT, MPP_POLL_BLOCK ) != MPP_OK )
    {
        std::cerr << "dec: no free input task\n";
        return -3;
    }
    MppTask task = nullptr;
    if ( mpi_->dequeue( ctx_, MPP_PORT_INPUT, &task ) != MPP_OK || !task )
    {
        std::cerr << "dec: input dequeue failed\n";
        return -4;
    }
    mpp_task_meta_set_packet( task, KEY_INPUT_PACKET, pkt_ );
    mpp_task_meta_set_frame( task, KEY_OUTPUT_FRAME, out_frame_ );

    MPP_RET ret = mpi_->enqueue( ctx_, MPP_PORT_INPUT, task );

    if ( ret != MPP_OK )
    {
        std::cerr << "dec: input enqueue ret=" << ret << '\n';
        // Best effort: hand the task back so the decoder thread is not stuck
        // waiting for it forever on the next poll.
        mpi_->enqueue( ctx_, MPP_PORT_INPUT, task );
        return -5;
    }

    // Output: wait for the decoded frame
    if ( mpi_->poll( ctx_, MPP_PORT_OUTPUT, MPP_POLL_BLOCK ) != MPP_OK )
    {
        std::cerr << "dec: output not ready\n";
        return -6;
    }
    task = nullptr;
    if ( mpi_->dequeue( ctx_, MPP_PORT_OUTPUT, &task ) != MPP_OK || !task )
    {
        std::cerr << "dec: output dequeue failed\n";
        return -7;
    }

    int fd = -1;
    MppFrame f = nullptr;
    mpp_task_meta_get_frame( task, KEY_OUTPUT_FRAME, &f );

    if ( !f )
    {
        std::cerr << "dec: output task WITHOUT frame in meta\n";
    }
    else if ( mpp_frame_get_info_change( f ) )
    {
        const uint32_t w = mpp_frame_get_width( f );
        const uint32_t h = mpp_frame_get_height( f );
        const size_t bs = mpp_frame_get_buf_size( f );
        std::cerr << "dec: info_change " << w << 'x' << h << " buf_size=" << bs << '\n';

        // Return the task BEFORE touching out_frame_
        mpi_->enqueue( ctx_, MPP_PORT_OUTPUT, task );

        mpp_frame_deinit( &out_frame_ );
        out_frame_ = nullptr;
        mpp_frame_init( &out_frame_ );
        mpp_frame_set_width( out_frame_, w );
        mpp_frame_set_height( out_frame_, h );
        mpp_frame_set_fmt( out_frame_, MPP_FMT_YUV422SP );
        MppBuffer b = nullptr;
        if ( mpp_buffer_get( out_group_, &b, bs ) != MPP_OK )
        {
            std::cerr << "dec: alloc " << bs << " bytes FAILED\n";
            mpi_->control( ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr );
            return -8;
        }
        mpp_frame_set_buffer( out_frame_, b );
        mpp_buffer_put( b );

        mpi_->control( ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr );
        return -1;  // the info_change frame itself is skipped — this is normal
    }
    else if ( mpp_frame_get_errinfo( f ) || mpp_frame_get_discard( f ) )
    {
        std::cerr << "dec: DECODE-ERROR errinfo=0x" << std::hex << mpp_frame_get_errinfo( f ) << std::dec
                  << " discard=" << mpp_frame_get_discard( f ) << " fmt=" << (int)mpp_frame_get_fmt( f ) << '\n';
    }
    else
    {
        MppBuffer buf = mpp_frame_get_buffer( f );
        if ( buf )
        {
            fd = mpp_buffer_get_fd( buf );
        }
        else
        {
            std::cerr << "dec: frame WITHOUT buffer\n";
        }
    }

    mpi_->enqueue( ctx_, MPP_PORT_OUTPUT, task );
    return fd;
}

int MppDecoder::aux_fd( size_t bytes )
{
    if ( !ok_ )
        return -1;
    if ( aux_ )
        return mpp_buffer_get_fd( aux_ );
    if ( mpp_buffer_get( out_group_, &aux_, bytes ) != MPP_OK )
        return -1;
    return mpp_buffer_get_fd( aux_ );
}
