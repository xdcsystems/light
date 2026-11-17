#include <ios>
#include <stdexcept>
#include <string>

#include "rknn_engine.hpp"

RknnEngine::RknnEngine( const char* model_path, const Log& log ) : log_( log )
{
    log_.info() << "loading model: " << model_path << '\n';

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
    parse_input_dims();

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
    release_io();
    rknn_destroy( ctx_ );
    ctx_ = 0;
}

void RknnEngine::parse_input_dims()
{
    in_w_ = 0;
    in_h_ = 0;
    in_c_ = 0;
    if ( in_attr_.n_dims != 4 )
        return;
    if ( in_attr_.fmt == RKNN_TENSOR_NHWC )
    {
        in_h_ = static_cast<int>( in_attr_.dims[1] );
        in_w_ = static_cast<int>( in_attr_.dims[2] );
        in_c_ = static_cast<int>( in_attr_.dims[3] );
    }
    else if ( in_attr_.fmt == RKNN_TENSOR_NCHW )
    {
        in_c_ = static_cast<int>( in_attr_.dims[1] );
        in_h_ = static_cast<int>( in_attr_.dims[2] );
        in_w_ = static_cast<int>( in_attr_.dims[3] );
    }
}

void RknnEngine::release_io()
{
    if ( !ctx_ )
        return;
    if ( in_mem_ )
    {
        rknn_destroy_mem( ctx_, in_mem_ );
        in_mem_ = nullptr;
    }
    for ( auto* m : out_mems_ )
    {
        if ( m )
            rknn_destroy_mem( ctx_, m );
    }
    out_mems_.clear();
}

Status RknnEngine::setup_outputs()
{
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
            log_.error() << "rknn_create_mem (output " << i << ") failed\n";
            return make_status( Errc::NoMemory, "rknn_create_mem failed" );
        }
        if ( const int rc = rknn_set_io_mem( ctx_, out_mems_[i], &out_attrs_[i] ); rc != RKNN_SUCC )
        {
            log_.error() << "rknn_set_io_mem (output " << i << ") failed\n";
            return sdk_status( Errc::Failed, "rknn_set_io_mem failed", rc );
        }
    }
    return {};
}

Status RknnEngine::setup_io()
{
    if ( !ctx_ )
        return make_status( Errc::Failed, "engine not initialized" );
    release_io();

    if ( in_attr_.n_dims != 4 || in_w_ <= 0 || in_h_ <= 0 || in_c_ <= 0 )
    {
        log_.warn() << "rknn: unsupported input tensor ndims=" << in_attr_.n_dims << " fmt=" << in_attr_.fmt << "\n";
        return make_status( Errc::Unsupported, "input tensor is not 4-D NHWC/NCHW" );
    }

    const rknn_tensor_type native_type = in_attr_.type;
    in_attr_.fmt = RKNN_TENSOR_NHWC;
    in_attr_.type = RKNN_TENSOR_UINT8;
    in_attr_.pass_through = 0;
    in_attr_.w_stride = 0;
    in_attr_.h_stride = 0;

    const uint32_t packed_u8 = static_cast<uint32_t>( in_w_ ) * static_cast<uint32_t>( in_h_ ) * static_cast<uint32_t>( in_c_ );
    const uint32_t raw_sz = in_attr_.size_with_stride ? in_attr_.size_with_stride : in_attr_.size;
    const uint32_t in_size = ( native_type == RKNN_TENSOR_UINT8 || native_type == RKNN_TENSOR_INT8 )
                                 ? ( raw_sz >= packed_u8 ? raw_sz : packed_u8 )
                                 : packed_u8;
    in_attr_.size = in_size;

    in_mem_ = rknn_create_mem( ctx_, in_size );
    if ( !in_mem_ )
    {
        log_.error() << "rknn_create_mem (input) failed\n";
        return make_status( Errc::NoMemory, "rknn_create_mem failed" );
    }

    if ( const int rc = rknn_set_io_mem( ctx_, in_mem_, &in_attr_ ); rc != RKNN_SUCC )
    {
        log_.error() << "rknn_set_io_mem (input) failed\n";
        release_io();
        return sdk_status( Errc::Failed, "rknn_set_io_mem failed", rc );
    }

    if ( Status st = setup_outputs(); !st.ok() )
    {
        release_io();
        return st;
    }

    log_.info() << "model input: " << in_w_ << 'x' << in_h_ << ' '
                << get_type_string( in_attr_.type ) << " zp=" << in_attr_.zp << " scale=" << in_attr_.scale
                << ", outputs: " << out_attrs_.size() << "\n";
    for ( size_t i = 0; i < out_attrs_.size(); ++i )
    {
        log_.info() << "  out[" << i << "]: " << out_attrs_[i].n_elems << " elems, " << get_type_string( out_attrs_[i].type ) << "\n";
    }

    return {};
}

Status RknnEngine::run() const
{
    if ( !ctx_ )
        return make_status( Errc::Failed, "engine not initialized" );
    const int rc = rknn_run( ctx_, nullptr );
    return rc == RKNN_SUCC ? Status{} : sdk_status( Errc::Failed, "rknn_run failed", rc );
}

Status RknnEngine::set_core_mask( unsigned mask ) const
{
    if ( !ctx_ )
        return make_status( Errc::Failed, "engine not initialized" );

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
                return {};  // AUTO by design (--core 0): keep the runtime default, no call
            // Anything else is a caller bug: silently skipping the pin
            // would hide it behind a working-looking return of 0.
            log_.warn() << "rknn_set_core_mask: invalid mask 0x" << std::hex << mask << std::dec << ", pin skipped\n";
            return make_status( Errc::Invalid, "invalid core mask" );
    }
    if ( const int rc = rknn_set_core_mask( ctx_, static_cast<rknn_core_mask>( mask ) ); rc != RKNN_SUCC )
    {
        log_.warn() << "rknn_set_core_mask(0x" << std::hex << mask << std::dec << ") failed\n";
        return sdk_status( Errc::Failed, "rknn_set_core_mask failed", rc );
    }
    return {};
}
