#include <fcntl.h>
#include <ios>
#include <linux/dma-heap.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rknn_engine.hpp"
#include "core/rga_limits.hpp"  // rgb888_stride_ok: the input rows every consumer reads

RknnEngine::RknnEngine( const char* model_path, const Log& log ) : log_( log )
{
    log_.info() << "loading model: " << model_path << '\n';

    // rknn_init takes the model as a non-const void*; with size 0 it is the .rknn path, which it only reads.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,custom-no-const-cast): SDK signature, read-only path
    if ( rknn_init( &ctx_, const_cast<char*>( model_path ), 0, 0, nullptr ) != RKNN_SUCC )
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

        return;
    }

    if ( in_attr_.fmt == RKNN_TENSOR_NCHW )
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
    for ( auto* m : in_mems_ )
    {
        if ( m )
            rknn_destroy_mem( ctx_, m );
    }
    in_mems_.clear();
    for ( const Dma32Buf& b : in_dma_ )
    {
        if ( b.ptr != nullptr )
            ::munmap( b.ptr, b.size );
        if ( b.fd >= 0 )
            ::close( b.fd );
    }
    in_dma_.clear();
    for ( auto* m : out_mems_ )
    {
        if ( m )
            rknn_destroy_mem( ctx_, m );
    }
    out_mems_.clear();
    in_stride_bytes_ = 0;
}

namespace
{
    int alloc_dma32_buf( size_t aligned, void*& out_ptr ) noexcept
    {
        out_ptr = nullptr;
        const int heap = ::open( "/dev/dma_heap/system-dma32", O_RDWR | O_CLOEXEC );
        if ( heap < 0 )
            return -1;
        dma_heap_allocation_data req {};
        req.len = aligned;
        req.fd_flags = O_RDWR | O_CLOEXEC;
        const int rc = ::ioctl( heap, DMA_HEAP_IOCTL_ALLOC, &req );
        ::close( heap );
        if ( rc < 0 )
            return -1;
        const int fd = static_cast<int>( req.fd );
        void* const ptr = ::mmap( nullptr, aligned, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
        if ( ptr == reinterpret_cast<void*>( -1 ) )
        {
            ::close( fd );
            return -1;
        }
        out_ptr = ptr;
        return fd;
    }

    uint32_t tensor_elem_bytes( rknn_tensor_type tp ) noexcept
    {
        switch ( tp )
        {
            case RKNN_TENSOR_FLOAT32:
                return sizeof( float );
            case RKNN_TENSOR_FLOAT16:
                return sizeof( uint16_t );
            default:
                return 1;
        }
    }
}  // namespace

Status RknnEngine::setup_outputs( bool keep_native_outputs )
{
    out_mems_.assign( out_attrs_.size(), nullptr );
    for ( size_t i = 0; i < out_attrs_.size(); ++i )
    {
        if ( !keep_native_outputs && out_attrs_[i].type != RKNN_TENSOR_FLOAT16 )
        {
            out_attrs_[i].type = RKNN_TENSOR_FLOAT16;
            out_attrs_[i].size = out_attrs_[i].n_elems * sizeof( uint16_t );
        }
        else if ( keep_native_outputs )
        {
            const uint32_t need = out_attrs_[i].n_elems * tensor_elem_bytes( out_attrs_[i].type );
            if ( out_attrs_[i].size < need )
                out_attrs_[i].size = need;
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

// Every consumer of the input tensor (RGA, GpuBoxDrawer, CalDumper,
// NetPreview) reads it as RGB888 rows of stride_bytes: whole pixels,
// 16-aligned for RGA. The row width comes from the queried w_stride
// (0 = tight) and is decided BEFORE allocation: setup_io sizes the buffers
// for it and binds an attr that carries it (was: w_stride zeroed first, so
// a model that pads its rows got undersized buffers and a tight
// stride_bytes). A layout no consumer can read fails here, at startup.
Status RknnEngine::input_row( int& row_px ) const
{
    if ( in_c_ != 3 )
    {
        log_.error() << "rknn: input has " << in_c_ << " channels, RGB888 (3) required\n";
        return make_status( Errc::Unsupported, "input tensor is not 3-channel RGB" );
    }
    row_px = static_cast<int>( in_attr_.w_stride ) > in_w_ ? static_cast<int>( in_attr_.w_stride ) : in_w_;
    const uint32_t row_bytes = static_cast<uint32_t>( row_px ) * 3U;
    if ( !rga_limits::rgb888_stride_ok( row_bytes, in_w_ ) )
    {
        log_.error() << "rknn: input row " << row_px << " px (w=" << in_w_ << ", w_stride=" << in_attr_.w_stride
                     << ") is not 16-aligned for RGA\n";
        return make_status( Errc::Unsupported, "input row stride not usable by RGA" );
    }
    return {};
}

Status RknnEngine::setup_io( bool keep_native_outputs, int input_slots )
{
    if ( !ctx_ )
        return make_status( Errc::Failed, "engine not initialized" );
    if ( input_slots <= 0 )
        return make_status( Errc::Invalid, "input_slots must be >= 1" );
    release_io();

    if ( in_attr_.n_dims != 4 || in_w_ <= 0 || in_h_ <= 0 || in_c_ <= 0 )
    {
        log_.warn() << "rknn: unsupported input tensor ndims=" << in_attr_.n_dims << " fmt=" << in_attr_.fmt << "\n";
        return make_status( Errc::Unsupported, "input tensor is not 4-D NHWC/NCHW" );
    }

    int row_px = 0;
    if ( Status st = input_row( row_px ); !st.ok() )
        return st;
    const uint32_t row_bytes = static_cast<uint32_t>( row_px ) * static_cast<uint32_t>( in_c_ );

    const rknn_tensor_type native_type = in_attr_.type;
    in_attr_.fmt = RKNN_TENSOR_NHWC;
    in_attr_.type = RKNN_TENSOR_UINT8;
    in_attr_.pass_through = 0;
    in_attr_.w_stride = row_px == in_w_ ? 0 : static_cast<uint32_t>( row_px );
    in_attr_.h_stride = 0;

    const uint32_t packed_u8 = row_bytes * static_cast<uint32_t>( in_h_ );
    const uint32_t raw_sz = in_attr_.size_with_stride ? in_attr_.size_with_stride : in_attr_.size;
    const uint32_t in_size = ( native_type == RKNN_TENSOR_UINT8 || native_type == RKNN_TENSOR_INT8 )
                                 ? ( raw_sz >= packed_u8 ? raw_sz : packed_u8 )
                                 : packed_u8;
    in_attr_.size = in_size;

    in_mems_.assign( static_cast<size_t>( input_slots ), nullptr );
    in_dma_.reserve( static_cast<size_t>( input_slots ) );
    bool warned_fallback = false;
    for ( int s = 0; s < input_slots; ++s )
    {
        if ( Status st = alloc_input_slot( s, in_size, warned_fallback ); !st.ok() )
        {
            release_io();
            return st;
        }
    }

    if ( Status st = bind_input_slot( 0 ); !st.ok() )
    {
        log_.error() << "rknn_set_io_mem (input) failed\n";
        release_io();
        return st;
    }

    if ( Status st = setup_outputs( keep_native_outputs ); !st.ok() )
    {
        release_io();
        return st;
    }

    in_stride_bytes_ = row_bytes;

    log_.info() << "model input: " << in_w_ << 'x' << in_h_ << ' '
                << get_type_string( in_attr_.type ) << " zp=" << in_attr_.zp << " scale=" << in_attr_.scale
                << ", outputs: " << out_attrs_.size() << "\n";
    for ( size_t i = 0; i < out_attrs_.size(); ++i )
    {
        log_.info() << "  out[" << i << "]: " << out_attrs_[i].n_elems << " elems, " << get_type_string( out_attrs_[i].type ) << "\n";
    }

    return {};
}

Status RknnEngine::alloc_input_slot( int slot, uint32_t in_size, bool& warned_fallback )
{
    const size_t aligned = ( static_cast<size_t>( in_size ) + 4095 ) & ~static_cast<size_t>( 4095 );
    void* ptr = nullptr;
    const int fd = alloc_dma32_buf( aligned, ptr );
    if ( fd >= 0 )
    {
        rknn_tensor_mem* const mem = rknn_create_mem_from_fd( ctx_, fd, ptr, in_size, 0 );
        if ( !mem )
        {
            ::munmap( ptr, aligned );
            ::close( fd );
            log_.error() << "rknn_create_mem_from_fd (input " << slot << ") failed\n";
            return make_status( Errc::NoMemory, "rknn_create_mem_from_fd failed" );
        }
        in_dma_.push_back( Dma32Buf { fd, ptr, aligned } );
        in_mems_[static_cast<size_t>( slot )] = mem;
        return {};
    }
    if ( !warned_fallback )
    {
        warned_fallback = true;
        log_.warn() << "rknn: /dev/dma_heap/system-dma32 unavailable, falling back to rknn_create_mem\n";
    }
    rknn_tensor_mem* const mem = rknn_create_mem( ctx_, in_size );
    if ( !mem )
    {
        log_.error() << "rknn_create_mem (input " << slot << ") failed\n";
        return make_status( Errc::NoMemory, "rknn_create_mem failed" );
    }
    in_mems_[static_cast<size_t>( slot )] = mem;
    return {};
}

Status RknnEngine::bind_input_slot( int slot ) noexcept
{
    if ( !ctx_ || slot < 0 || slot >= static_cast<int>( in_mems_.size() ) || !in_mems_[slot] )
        return make_status( Errc::Invalid, "invalid input slot" );
    const int rc = rknn_set_io_mem( ctx_, in_mems_[slot], &in_attr_ );
    return rc == RKNN_SUCC ? Status {} : sdk_status( Errc::Failed, "rknn_set_io_mem failed", rc );
}

TensorRef RknnEngine::input_tensor( int slot ) const noexcept
{
    if ( slot < 0 || slot >= static_cast<int>( in_mems_.size() ) || !in_mems_[slot] )
        return {};
    return { in_mems_[slot]->fd, in_mems_[slot]->virt_addr, in_mems_[slot]->size, in_stride_bytes_ };
}

void RknnEngine::sync_output( size_t i ) const noexcept
{
    if ( ctx_ && i < out_mems_.size() && out_mems_[i] )
        rknn_mem_sync( ctx_, out_mems_[i], RKNN_MEMORY_SYNC_FROM_DEVICE );
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
