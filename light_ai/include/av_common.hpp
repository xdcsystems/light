// Shared ffmpeg error formatting: replaces the repeated
// "char buf + av_strerror + cerr" triplet across net_* files.
#pragma once

#include <cstdint>
#include <ostream>

extern "C"
{
#include <libavutil/error.h>
}

struct AvErrText
{
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
};

inline AvErrText av_err_str( int ret ) noexcept
{
    AvErrText t;
    av_strerror( ret, t.buf, sizeof( t.buf ) );
    return t;
}

inline std::ostream& operator<<( std::ostream& os, const AvErrText& e )
{
    return os << static_cast<const char*>( e.buf );
}

inline void av_noop_free( void*, uint8_t* ) {}
