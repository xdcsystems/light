#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "core/fp16.hpp"
#include "cpu_affinity.hpp"
#include "reid_worker.hpp"

namespace
{
    // The NPU core of Re-ID, away from YOLO where it can be:
    //   pool > 1: YOLO pins one engine per core (0/1/2) -> AUTO (the runtime default);
    //   pool 1, --core 4 (YOLO on core 2) -> core 0;
    //   pool 1, --core 7 (YOLO on all three cores) -> AUTO;
    //   pool 1, any other --core (0 = AUTO, 1/2 = core 0/1, 3 = cores 0+1) -> core 2.
    unsigned choose_reid_core_mask( int pool_size, int infer_core ) noexcept
    {
        if ( pool_size > 1 )
            return static_cast<unsigned>( RKNN_NPU_CORE_AUTO );
        if ( infer_core == static_cast<int>( RKNN_NPU_CORE_2 ) )
            return static_cast<unsigned>( RKNN_NPU_CORE_0 );
        if ( infer_core == static_cast<int>( RKNN_NPU_CORE_0_1_2 ) )
            return static_cast<unsigned>( RKNN_NPU_CORE_AUTO );
        return static_cast<unsigned>( RKNN_NPU_CORE_2 );
    }

    RknnEngine make_reid_engine( const char* model_path, int pool_size, int infer_core, const Log& log, int& out_dim )
    {
        RknnEngine eng( model_path, log );
        const Status io_st = eng.setup_io( true, reid_cfg::kReidSlots );
        if ( !io_st.ok() )
            throw std::runtime_error( std::string( "reid: setup_io failed: " ) + io_st.what() );

        // Pool 1 avoids colliding with YOLO's configured NPU core; pool 3 uses NPU auto.
        const unsigned mask = choose_reid_core_mask( pool_size, infer_core );
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

        const int zp = attr.zp;
        const float sc = attr.scale;
        switch ( attr.type )
        {
            case RKNN_TENSOR_FLOAT32:
                std::memcpy( res.feat.data(), raw, static_cast<std::size_t>( dim ) * sizeof( float ) );
                break;
            case RKNN_TENSOR_FLOAT16:
            {
                const auto* fp16 = reinterpret_cast<const uint16_t*>( raw );
                std::transform( fp16, fp16 + dim, res.feat.begin(), fp16_to_float );
                break;
            }
            case RKNN_TENSOR_INT8:
                std::transform( raw, raw + dim, res.feat.begin(), [zp, sc]( int8_t q ) {
                    return static_cast<float>( static_cast<int>( q ) - zp ) * sc;
                } );
                break;
            case RKNN_TENSOR_UINT8:
            {
                const auto* u8 = reinterpret_cast<const uint8_t*>( raw );
                std::transform( u8, u8 + dim, res.feat.begin(), [zp, sc]( uint8_t q ) {
                    return static_cast<float>( static_cast<int>( q ) - zp ) * sc;
                } );
                break;
            }
            default: return false;
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

void ReidWorker::BigCorePin::operator()() const noexcept
{
    if ( log == nullptr )
        return;
    // Once at the reid worker thread start (BasicReidQueue::worker_loop, before its job loop), not the frame path.
    pin_current_thread_to_big_cores( "reid worker", *log );
}

ReidWorker::ReidWorker( const char* model_path, int pool_size, int infer_core, const Log& log )
  : engine_( make_reid_engine( model_path, pool_size, infer_core, log, feat_dim_ ) )
  , queue_( EngineRunner { &engine_ }, BigCorePin { &log } )
{
}
