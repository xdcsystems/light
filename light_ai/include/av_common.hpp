// Shared ffmpeg error formatting: replaces the repeated
// "char buf + av_strerror + cerr" triplet across net_* files.
#pragma once
#include <string>
#include <libavutil/error.h>

inline std::string av_err_str( int ret )
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror( ret, buf, sizeof( buf ) );
    return std::string( buf );
}
