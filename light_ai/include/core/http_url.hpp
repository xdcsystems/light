// parse_http_url: http://host[:port][/path] -> host, port (default 80), path
// (default "/"). Pure, host-tested (tests/test_http_url.cpp). The source of
// NetMjpegSource::setup_device; the Status texts are its error reasons.

#pragma once

#include <string>

#include "core/status.hpp"

struct HttpUrl
{
    std::string host;
    int port = 80;
    std::string path = "/";
};

// ok = out filled. Otherwise Errc::Invalid with the reason; out is unspecified.
Status parse_http_url( const std::string& url, HttpUrl& out );
