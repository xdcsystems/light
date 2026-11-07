#include <iostream>
#include <stdexcept>
#include <string>

#include "rknn_engine.hpp"

RknnEngine::RknnEngine( const char* model_path )
{
    std::cout << "loading model: " << model_path << '\n';

    if ( rknn_init( &ctx_, const_cast<char*>( model_path ), 0, 0, nullptr ) != RKNN_SUCC ) // NOLINT(cppcoreguidelines-pro-type-const-cast)
    {
        ctx_ = 0;
        throw std::runtime_error( std::string( "rknn_init failed for " ) + model_path );
    }

    rknn_input_output_num io_num {};
    if ( rknn_query( ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof( io_num ) ) != RKNN_SUCC || io_num.n_input < 1 || io_num.n_output < 1 )
    {
        rknn_destroy( ctx_ );
        ctx_ = 0;
        throw std::runtime_error( "rknn_query IN_OUT_NUM failed" );
    }

    in_attr_ = {};
    in_attr_.index = 0;
    if ( rknn_query( ctx_, RKNN_QUERY_INPUT_ATTR, &in_attr_, sizeof( in_attr_ ) ) != RKNN_SUCC )
    {
        rknn_destroy( ctx_ );
        ctx_ = 0;
        throw std::runtime_error( "rknn_query INPUT_ATTR failed" );
    }

    out_attrs_.resize( io_num.n_output );
    for ( uint32_t i = 0; i < io_num.n_output; ++i )
    {
        out_attrs_[i] = {};
        out_attrs_[i].index = i;
        if ( rknn_query( ctx_, RKNN_QUERY_OUTPUT_ATTR, &out_attrs_[i], sizeof( out_attrs_[i] ) ) != RKNN_SUCC )
        {
            rknn_destroy( ctx_ );
            ctx_ = 0;
            throw std::runtime_error( "rknn_query OUTPUT_ATTR[" + std::to_string( i ) + "] failed" );
        }
    }
}

RknnEngine::~RknnEngine()
{
    if ( !ctx_ )
        return;
    if ( in_mem_ )
        rknn_destroy_mem( ctx_, in_mem_ );
    for ( auto* m : out_mems_ )
    {
        if ( m )
            rknn_destroy_mem( ctx_, m );
    }
    rknn_destroy( ctx_ );
    ctx_ = 0;
}

int RknnEngine::setup_io()
{
    if ( !ctx_ )
        return -1;

    in_attr_.fmt = RKNN_TENSOR_NHWC;
    in_attr_.type = RKNN_TENSOR_UINT8;
    in_attr_.pass_through = 0;
    in_attr_.w_stride = 0;
    in_attr_.h_stride = 0;

    const uint32_t in_size = in_attr_.size_with_stride ? in_attr_.size_with_stride : in_attr_.size;
    in_mem_ = rknn_create_mem( ctx_, in_size );
    if ( !in_mem_ )
    {
        std::cerr << "rknn_create_mem (input) failed\n";
        return -1;
    }

    if ( rknn_set_io_mem( ctx_, in_mem_, &in_attr_ ) != RKNN_SUCC )
    {
        std::cerr << "rknn_set_io_mem (input) failed\n";
        return -1;
    }

    // The output buffers are sized exactly as the model expects. For an INT8
    // output this is n_elems bytes, not n_elems * sizeof(float) — the old
    // x4 over-allocation worked only by luck of the runtime accepting it.
    out_mems_.assign( out_attrs_.size(), nullptr );
    for ( size_t i = 0; i < out_attrs_.size(); ++i )
    {
        if ( out_attrs_[i].type != RKNN_TENSOR_FLOAT16 )
        {
            out_attrs_[i].type = RKNN_TENSOR_FLOAT16;
            out_attrs_[i].size = out_attrs_[i].n_elems * sizeof( uint16_t );
        }
        out_mems_[i] = rknn_create_mem( ctx_, out_attrs_[i].size );
        if ( !out_mems_[i] )
        {
            std::cerr << "rknn_create_mem (output " << i << ") failed\n";
            return -1;
        }
        if ( rknn_set_io_mem( ctx_, out_mems_[i], &out_attrs_[i] ) != RKNN_SUCC )
        {
            std::cerr << "rknn_set_io_mem (output " << i << ") failed\n";
            return -1;
        }
    }

    // NHWC: dims = {N, H, W, C}; print W x H honestly.
    std::cout << "model input: " << in_attr_.dims[in_attr_.n_dims - 2] << 'x' << in_attr_.dims[in_attr_.n_dims - 3] << ' '
              << get_type_string( in_attr_.type ) << " zp=" << in_attr_.zp << " scale=" << in_attr_.scale
              << ", outputs: " << out_attrs_.size() << "\n";
    for ( size_t i = 0; i < out_attrs_.size(); ++i )
    {
        std::cout << "  out[" << i << "]: " << out_attrs_[i].n_elems << " elems, " << get_type_string( out_attrs_[i].type ) << "\n";
    }

    return 0;
}

int RknnEngine::run() const
{
    return rknn_run( ctx_, nullptr ) == RKNN_SUCC ? 0 : -1;
}

int RknnEngine::set_core_mask( unsigned mask ) const
{
    if ( !ctx_ )
        return -1;

    // M4: the accepted values are the numeric flags of rknn_api.h:
    // CORE_0=1, CORE_1=2, CORE_0_1=3, CORE_2=4, CORE_0_1_2=7
    // (see rknn_api.h lines 245-250).
    //  - 1/2/4 are the per-core flags — the pool pins one engine per core with these;
    //  - 3/7 are the pair/triple masks of the single-context path.
    // So the switch is only a whitelist: 0 = AUTO keeps the runtime default (no call),
    // any other non-whitelisted value is a caller bug: loud log, -1.

    switch ( mask )
    {
        case 1:  // RKNN_NPU_CORE_0
        case 2:  // RKNN_NPU_CORE_1
        case 3:  // RKNN_NPU_CORE_0_1
        case 4:  // RKNN_NPU_CORE_2
        case 7:  // RKNN_NPU_CORE_0_1_2
            break;
        default:
            if ( mask == 0 )
                return 0;  // AUTO by design (--core 0): keep the runtime default, no call
            // Anything else is a caller bug: silently skipping the pin
            // would hide it behind a working-looking return of 0.
            std::cerr << "rknn_set_core_mask: invalid mask 0x" << std::hex << mask << std::dec << ", pin skipped\n";
            return -1;
    }
    if ( rknn_set_core_mask( ctx_, static_cast<rknn_core_mask>( mask ) ) != RKNN_SUCC )
    {
        std::cerr << "rknn_set_core_mask(0x" << std::hex << mask << std::dec << ") failed\n";
        return -1;
    }
    return 0;
}
