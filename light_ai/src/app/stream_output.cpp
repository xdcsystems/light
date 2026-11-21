#include "app/stream_output.hpp"

StreamOutput::StreamOutput( const Log& log ) : sink_( log ), rep_( log ) {}

void StreamOutput::init( const StreamConfig& stream, const TensorSet& tensors, int model_w, int model_h,
                         const LbGeom& enc_lb, int nsrc )
{
    fps_ = stream.fps;
    sink_.init( stream, tensors, model_w, model_h, enc_lb, nsrc );
}

void StreamOutput::flush_dirty() noexcept
{
    if ( sink_.enabled() )
        sink_.flush_dirty();
}

double StreamOutput::publish_frame( int frame, const FrameView& view, const DetList& dets, const TrajList& trajs,
                                    const Tracker& tracker, int nsrc, bool defer_enc ) noexcept
{
    rep_.report( frame, dets, trajs, view.src_idx, nsrc );
    if ( !sink_.enabled() )
        return 0.0;
    return sink_.draw_and_publish( view, dets, trajs, tracker, defer_enc );
}

void StreamOutput::summary( int frames, double elapsed_sec, int reconnects ) const noexcept
{
    rep_.summary( frames, elapsed_sec, reconnects );
}
