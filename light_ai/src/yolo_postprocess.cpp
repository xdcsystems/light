#include <algorithm>
#include <array>
#include <cstdint>

#include "yolo_postprocess.hpp"
#include "infer_slot.hpp"  // InferJob
#include "rknn_engine.hpp"

void YoloDecode::operator()( const RknnEngine& npu, const InferJob& job, DetList& out )
{
    // NCHW: dims = {1, 255, h, w}. The index of a head is kept (an unusable
    // one stays a null view): the decoder's messages name the head by it.
    std::array<YoloHead, YoloDecoder::kMaxHeads> heads {};
    const size_t n = std::min( npu.output_attrs().size(), heads.size() );
    for ( size_t gi = 0; gi < n; ++gi )
    {
        const auto& attr = npu.output_attrs()[gi];
        const uint16_t* data = reinterpret_cast<const uint16_t*>( npu.output_data( gi ) );
        if ( !data || attr.n_dims != 4 )
            continue;
        heads[gi] = YoloHead { data, static_cast<int>( attr.dims[1] ), static_cast<int>( attr.dims[2] ),
                               static_cast<int>( attr.dims[3] ) };
    }
    const YoloFrame f { static_cast<float>( job.src_w ), static_cast<float>( job.src_h ),
                        static_cast<float>( npu.input_width() ), static_cast<float>( npu.input_height() ), job.lb };
    dec_.decode( heads.data(), static_cast<int>( n ), f, out );
}
