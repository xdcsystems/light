#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <libdrm/drm_fourcc.h>  // DRM_FORMAT_*

extern "C"
{
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "av_common.hpp"  // av_noop_free
#include "net/encoder_frame_pool.hpp"

EncoderFramePool::EncoderFramePool( const StreamLayout& layout )
{
    // Alloc failures throw from here (fail loud, RknnEngine pattern); the
    // dma-buf pool has already self-cleaned by then.
    std::string why;
    if ( !ring_.alloc( static_cast<size_t>( layout.w ) * layout.vstride * 3 / 2, MODULE_TAG, __FUNCTION__, &why ) )
        throw std::runtime_error( "net encoder: " + why );

    for ( int i = 0; i < kSlots; ++i )
    {
        MppBuffer mb = ring_.buf[i];
        AVDRMFrameDescriptor& desc = desc_[i];
        desc.nb_objects = 1;
        desc.objects[0].fd = mpp_buffer_get_fd( mb );
        desc.objects[0].size = mpp_buffer_get_size( mb );
        desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
        desc.nb_layers = 1;
        desc.layers[0].format = DRM_FORMAT_NV12;
        desc.layers[0].nb_planes = 2;
        desc.layers[0].planes[0].object_index = 0;
        desc.layers[0].planes[0].offset = 0;
        desc.layers[0].planes[0].pitch = layout.w;
        // The VU plane at w * vstride: where the RGA writes land (explicit strides).
        desc.layers[0].planes[1].object_index = 0;
        desc.layers[0].planes[1].offset = static_cast<ptrdiff_t>( layout.w ) * layout.vstride;
        desc.layers[0].planes[1].pitch = layout.w;

        AVFrame* fr = av_frame_alloc();
        frames_[i] = fr;
        if ( fr )
        {
            fr->format = AV_PIX_FMT_DRM_PRIME;
            fr->width = layout.w;
            fr->height = layout.h;
            fr->data[0] = reinterpret_cast<uint8_t*>( &desc );
            fr->buf[0] = av_buffer_create( reinterpret_cast<uint8_t*>( &desc ), sizeof( desc ), av_noop_free, nullptr, 0 );
        }
        if ( !fr || !fr->buf[0] )
        {
            release();  // the dtor does not run for a throwing ctor; ring_ frees itself
            throw std::runtime_error( "net encoder: av_frame_alloc / av_buffer_create failed" );
        }
    }
}

EncoderFramePool::~EncoderFramePool()
{
    release();
}

void EncoderFramePool::release()
{
    for ( AVFrame*& fr : frames_ )
    {
        if ( fr )
            av_frame_free( &fr );
    }
}
