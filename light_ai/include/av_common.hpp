// Shared ffmpeg error formatting: replaces the repeated
// "char buf + av_strerror + cerr" triplet across net_* files.
#pragma once

#include <cstdint>
#include <string>

extern "C"
{
#include <libavutil/error.h>
}

inline std::string av_err_str( int ret )
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror( ret, buf, sizeof( buf ) );
    return std::string( buf );
}

inline void av_noop_free( void*, uint8_t* ) {}
