// parse_http_url: http://host[:port][/path] -> host, port (default 80), path
// (default "/"). Pure, host-tested (tests/test_http_url.cpp). The source of
// NetMjpegSource::setup_device; the texts in why are its error reasons.

#pragma once

#include <string>

struct HttpUrl
{
    std::string host;
    int port = 80;
    std::string path = "/";
};

// true = out filled. false: why (when non-null) gets the reason, out is unspecified.
bool parse_http_url( const std::string& url, HttpUrl& out, std::string* why );
