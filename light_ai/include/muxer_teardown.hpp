// Shared muxer teardown for session-scoped AVFormatContexts.
#pragma once

#include <libavformat/avformat.h>

// The muxer context's custom AVIOContext is owned by the transport
// (TcpPullServer::close_client or avio_closep in the UDP branch), so
// it must be detached before the context is freed -- otherwise the
// same avio context is closed twice. Used by NetEncoder::FcGuard and
// NetPreview's NutSessionGuard.
inline void teardown_muxer( AVFormatContext* fc )
{
    if ( fc )
    {
        fc->pb = nullptr;
        avformat_free_context( fc );
    }
}
