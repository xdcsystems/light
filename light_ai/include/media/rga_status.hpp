// rga_check: the im2d status check shared by the RGA users (RgaResizer,
// FrameComposer). It needs the RGA SDK headers: not for core/ or the host tests.

#pragma once

#include <cstddef>  // NULL and memset: the im2d headers and macros use them without including
#include <cstring>

#include <im2d.h>

#include "core/log.hpp"
#include "core/status.hpp"

// The only expansion of imStrError: an im2d status <= 0 is a failure,
// logged as "<what>: <the SDK's text>" and returned as Failed with the
// raw IM_STATUS in sys(). what: a string literal (Status keeps the pointer).
inline Status rga_check( IM_STATUS st, const Log& log, const char* what )
{
    if ( static_cast<int>( st ) > 0 )
        return {};
    log.error() << what << ": " << imStrError( st ) << '\n';
    return sdk_status( Errc::Failed, what, static_cast<int>( st ) );
}
