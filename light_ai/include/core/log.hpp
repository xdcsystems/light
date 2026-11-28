// Log: the diagnostic output of the process, injected by reference instead
// of reaching for std::cout / std::cerr directly.
//
// In tests (and early CLI parsing), Log writes synchronously under a
// recursive mutex to the caller-supplied std::ostream pair. In production,
// Log formats each line into a fixed stack buffer (zero heap, zero mutex)
// and hands it to a lock-free AsyncLogSink via LogSinkFn.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/fixed_fmt.hpp"
#include "core/non_copyable.hpp"

struct LogSinkFn
{
    void* ctx = nullptr;
    void ( *write )( void* ctx, bool is_err, const char* data, std::size_t len ) noexcept = nullptr;
};

class Log : private NonMovable<Log>
{
public:
    enum class Level : uint8_t
    {
        Debug,
        Info,
        Warn,
        Error,
        Off  // threshold only: silences everything
    };

    // One statement's worth of output. Created only by Log; lives until the
    // end of the full expression (guaranteed copy elision: never copied or moved).
    class Line : private NonMovable<Line>
    {
    public:
        enum
        {
            kBufSize = 512
        };

    public:
        // Sync mode (os != nullptr): holds mx_ until destruction.
        Line( std::ostream* os, std::recursive_mutex& mx ) : os_( os )
        {
            if ( os_ )
                lk_ = std::unique_lock<std::recursive_mutex>( mx );
        }
        // Lock-free async mode: formats into stack buf_ and flushes in ~Line().
        Line( LogSinkFn sink, bool is_err, bool active ) noexcept
          : sink_( active ? sink : LogSinkFn {} ), is_err_( is_err )
        {
        }

        ~Line()
        {
            if ( sink_.write != nullptr && len_ > 0 )
                sink_.write( sink_.ctx, is_err_, buf_.data(), len_ );
        }

        Line& operator<<( char v )
        {
            if ( sink_.write != nullptr )
            {
                append_char( v );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& operator<<( signed char v ) { return write_signed( v ); }
        Line& operator<<( unsigned char v ) { return write_unsigned( v ); }

        Line& operator<<( bool v )
        {
            if ( sink_.write != nullptr )
            {
                append_char( v ? '1' : '0' );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& operator<<( const char* v )
        {
            if ( sink_.write != nullptr )
            {
                if ( v != nullptr )
                    append_bytes( v, std::strlen( v ) );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& operator<<( std::string_view v )
        {
            if ( sink_.write != nullptr )
            {
                append_bytes( v.data(), v.size() );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& operator<<( const std::string& v )
        {
            if ( sink_.write != nullptr )
            {
                append_bytes( v.data(), v.size() );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& operator<<( Fixed f )
        {
            if ( sink_.write != nullptr )
            {
                append_fixed( f.v, f.prec );
                return *this;
            }
            if ( os_ )
                *os_ << f;
            return *this;
        }

        Line& operator<<( short v ) { return write_signed( v ); }
        Line& operator<<( unsigned short v ) { return write_unsigned( v ); }
        Line& operator<<( int v ) { return write_signed( v ); }
        Line& operator<<( long v ) { return write_signed( v ); }
        Line& operator<<( long long v ) { return write_signed( v ); }
        Line& operator<<( unsigned int v ) { return write_unsigned( v ); }
        Line& operator<<( unsigned long v ) { return write_unsigned( v ); }
        Line& operator<<( unsigned long long v ) { return write_unsigned( v ); }
        Line& operator<<( float v ) { return write_float( static_cast<double>( v ) ); }
        Line& operator<<( double v ) { return write_float( v ); }

        Line& operator<<( const void* v )
        {
            if ( sink_.write != nullptr )
            {
                append_bytes( "0x", 2 );
                const bool saved_hex = hex_;
                hex_ = true;
                append_uint( reinterpret_cast<uintptr_t>( v ) );
                hex_ = saved_hex;
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
        Line& operator<<( T v )
        {
            return *this << static_cast<std::underlying_type_t<T>>( v );
        }

        // Stream manipulators (std::hex, std::dec, std::fixed, ...).
        Line& operator<<( std::ios_base& ( *manip )( std::ios_base& ) )
        {
            if ( sink_.write != nullptr )
            {
                if ( manip == static_cast<std::ios_base& ( * )( std::ios_base& )>( std::hex ) )
                    hex_ = true;
                if ( manip == static_cast<std::ios_base& ( * )( std::ios_base& )>( std::dec ) )
                    hex_ = false;
                return *this;
            }
            if ( os_ )
                *os_ << manip;
            return *this;
        }

        Line& operator<<( std::ostream& ( *manip )( std::ostream& ) )
        {
            if ( sink_.write != nullptr )
            {
                if ( manip == static_cast<std::ostream& ( * )( std::ostream& )>( std::endl ) )
                    append_char( '\n' );
                return *this;
            }
            if ( os_ )
                *os_ << manip;
            return *this;
        }

    private:
        void append_bytes( const char* s, std::size_t n ) noexcept
        {
            if ( s == nullptr || n == 0 || len_ >= kBufSize )
                return;
            const std::size_t avail = kBufSize - len_;
            const std::size_t copy_n = std::min( n, avail );
            std::memcpy( buf_.data() + len_, s, copy_n );
            len_ += copy_n;
        }

        void append_char( char c ) noexcept
        {
            if ( len_ < kBufSize )
            {
                buf_[len_] = c;
                ++len_;
            }
        }

        void append_uint( unsigned long long v ) noexcept
        {
            std::array<char, 24> tmp {};
            std::size_t n = 0;
            const unsigned long long base = hex_ ? 16ULL : 10ULL;
            do
            {
                const unsigned d = static_cast<unsigned>( v % base );
                tmp[n++] = static_cast<char>( d < 10 ? ( '0' + d ) : ( 'a' + ( d - 10 ) ) );
                v /= base;
            } while ( v != 0 && n < tmp.size() );
            while ( n > 0 )
                append_char( tmp[--n] );
        }

        void append_int( long long v ) noexcept
        {
            if ( hex_ )
            {
                append_uint( static_cast<unsigned long long>( v ) );
                return;
            }
            if ( v < 0 )
            {
                append_char( '-' );
                append_uint( 0ULL - static_cast<unsigned long long>( v ) );
                return;
            }
            append_uint( static_cast<unsigned long long>( v ) );
        }

        void append_fixed( double v, int prec ) noexcept
        {
            if ( !std::isfinite( v ) )
            {
                append_bytes( std::isnan( v ) ? "nan" : ( v < 0 ? "-inf" : "inf" ),
                              std::isnan( v ) ? 3 : ( v < 0 ? 4 : 3 ) );
                return;
            }
            if ( v < 0.0 )
            {
                append_char( '-' );
                v = -v;
            }
            const int p = std::clamp( prec, 0, 6 );
            double scale = 1.0;
            for ( int i = 0; i < p; ++i )
                scale *= 10.0;
            if ( v * scale >= 9.0e18 )
            {
                append_bytes( "ovf", 3 );
                return;
            }
            const unsigned long long scaled = static_cast<unsigned long long>( std::llround( v * scale ) );
            const unsigned long long whole = scaled / static_cast<unsigned long long>( scale );
            const bool saved_hex = hex_;
            hex_ = false;
            append_uint( whole );
            if ( p > 0 )
            {
                append_char( '.' );
                unsigned long long frac = scaled % static_cast<unsigned long long>( scale );
                std::array<char, 8> fbuf {};
                for ( int i = p - 1; i >= 0; --i )
                {
                    fbuf[static_cast<std::size_t>( i )] = static_cast<char>( '0' + ( frac % 10ULL ) );
                    frac /= 10ULL;
                }
                append_bytes( fbuf.data(), static_cast<std::size_t>( p ) );
            }
            hex_ = saved_hex;
        }

        Line& write_signed( long long v )
        {
            if ( sink_.write != nullptr )
            {
                append_int( v );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& write_unsigned( unsigned long long v )
        {
            if ( sink_.write != nullptr )
            {
                append_uint( v );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

        Line& write_float( double v )
        {
            if ( sink_.write != nullptr )
            {
                append_fixed( v, 3 );
                return *this;
            }
            if ( os_ )
                *os_ << v;
            return *this;
        }

    private:
        std::ostream* os_ = nullptr;
        std::unique_lock<std::recursive_mutex> lk_;
        LogSinkFn sink_ {};
        bool is_err_ = false;
        bool hex_ = false;
        std::size_t len_ = 0;
        std::array<char, kBufSize> buf_ {};
    };

    // out/err must outlive the Log. min_level: lines below it are dropped.
    Log( std::ostream& out, std::ostream& err, Level min_level = Level::Info )
      : out_( &out ), err_( &err ), min_( min_level )
    {
    }
    // Async lock-free mode: sink must outlive the Log.
    explicit Log( LogSinkFn sink, Level min_level = Level::Info ) : sink_( sink ), min_( min_level ) {}

    ~Log() = default;

    Line debug() const { return line( Level::Debug, false ); }
    Line info() const { return line( Level::Info, false ); }
    Line warn() const { return line( Level::Warn, true ); }
    Line error() const { return line( Level::Error, true ); }

    bool enabled( Level level ) const { return level >= min_; }

private:
    Line line( Level level, bool is_err ) const
    {
        const bool active = enabled( level );
        if ( sink_.write != nullptr )
            return { sink_, is_err, active };
        std::ostream* os = is_err ? err_ : out_;
        return { active ? os : nullptr, mx_ };
    }

private:
    std::ostream* out_ = nullptr;
    std::ostream* err_ = nullptr;
    LogSinkFn sink_ {};
    const Level min_;
    mutable std::recursive_mutex mx_;  // mutable: logging through a const Log& is the normal use
};
