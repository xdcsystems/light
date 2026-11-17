#include <cstring>
#include <stdexcept>
#include <string>

#include "core/fp16.hpp"
#include "reid_worker.hpp"

namespace
{
    RknnEngine make_reid_engine( const char* model_path, int pool_size, const Log& log, int& out_dim )
    {
        RknnEngine eng( model_path, log );
        const Status io_st = eng.setup_io( true, reid_cfg::kReidSlots );
        if ( !io_st.ok() )
            throw std::runtime_error( std::string( "reid: setup_io failed: " ) + io_st.what() );

        // Pool 1 keeps YOLO on core 0/1 and pins Re-ID to core 2; pool 3 uses NPU auto.
        const unsigned mask = ( pool_size <= 1 ) ? 4u : 0u;
        static_cast<void>( eng.set_core_mask( mask ) );

        if ( eng.input_width() < 32 || eng.input_height() < 32 )
            throw std::runtime_error( "reid: invalid model input geometry" );

        const auto& outs = eng.output_attrs();
        if ( outs.size() != 1 || outs[0].n_elems == 0 || outs[0].n_elems > reid_cfg::kReidDimMax )
            throw std::runtime_error( "reid: unsupported output tensor count or dimension" );

        out_dim = static_cast<int>( outs[0].n_elems );
        log.info() << "reid: active (" << eng.input_width() << 'x' << eng.input_height()
                   << " -> " << out_dim << "-D, slots=" << reid_cfg::kReidSlots
                   << ", core_mask=0x" << std::hex << mask << std::dec << ")\n";
        return eng;
    }

    bool extract_embedding( const rknn_tensor_attr& attr, const int8_t* raw, ReidResult& res ) noexcept
    {
        if ( !raw )
            return false;
        const int dim = static_cast<int>( attr.n_elems );
        if ( dim <= 0 || dim > reid_cfg::kReidDimMax )
            return false;

        if ( attr.type == RKNN_TENSOR_FLOAT32 )
        {
            std::memcpy( res.feat.data(), raw, static_cast<std::size_t>( dim ) * sizeof( float ) );
        }
        else if ( attr.type == RKNN_TENSOR_FLOAT16 )
        {
            const auto* fp16 = reinterpret_cast<const uint16_t*>( raw );
            for ( int i = 0; i < dim; ++i )
                res.feat[i] = fp16_to_float( fp16[i] );
        }
        else if ( attr.type == RKNN_TENSOR_INT8 )
        {
            for ( int i = 0; i < dim; ++i )
                res.feat[i] = static_cast<float>( static_cast<int>( raw[i] ) - attr.zp ) * attr.scale;
        }
        else if ( attr.type == RKNN_TENSOR_UINT8 )
        {
            const auto* u8 = reinterpret_cast<const uint8_t*>( raw );
            for ( int i = 0; i < dim; ++i )
                res.feat[i] = static_cast<float>( static_cast<int>( u8[i] ) - attr.zp ) * attr.scale;
        }
        else
        {
            return false;
        }

        res.dim = dim;
        return l2_normalize( res.feat.data(), dim );
    }

}  // namespace

bool ReidWorker::EngineRunner::operator()( const ReidJob& job, ReidResult& res ) const noexcept
{
    if ( !engine )
        return false;
    if ( !engine->bind_input_slot( job.slot ).ok() )
        return false;
    if ( !engine->run().ok() )
        return false;
    engine->sync_output( 0 );
    const auto& outs = engine->output_attrs();
    if ( outs.empty() )
        return false;
    return extract_embedding( outs[0], engine->output_data( 0 ), res );
}

ReidWorker::ReidWorker( const char* model_path, int pool_size, const Log& log )
  : engine_( make_reid_engine( model_path, pool_size, log, feat_dim_ ) )
  , queue_( EngineRunner { &engine_ } )
{
}
