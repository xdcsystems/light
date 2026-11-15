#include <cerrno>
#include <cstring>
#include <string>

#include "check.hpp"
#include "core/http_url.hpp"
#include "suites.hpp"

namespace
{
    bool invalid_is( const Status& s, const char* why )
    {
        return s.code() == Errc::Invalid && std::strcmp( s.what(), why ) == 0;
    }
}  // namespace

void test_http_url( TestRun& t )
{
    HttpUrl u;
    CHECK( t, parse_http_url( "http://192.168.1.2:8080/video", u ).ok() );
    CHECK( t, u.host == "192.168.1.2" && u.port == 8080 && u.path == "/video" );
    CHECK( t, parse_http_url( "http://cam.local", u ).ok() );
    CHECK( t, u.host == "cam.local" && u.port == 80 && u.path == "/" );
    CHECK( t, parse_http_url( "http://h:65535/a/b?x=1", u ).ok() && u.port == 65535 && u.path == "/a/b?x=1" );
    // A stale errno must not reject a valid port: strtol touches it on
    // overflow only, and nothing zeroes it before the call.
    errno = EINVAL;
    CHECK( t, parse_http_url( "http://h:8080/video", u ).ok() && u.port == 8080 );
    CHECK( t, invalid_is( parse_http_url( "https://h/", u ), "url must start with http://" ) );
    for ( const char* bad : { "http://h:0/", "http://h:65536/", "http://h:/", "http://h:80x/", "http://h:-1/" } )
        CHECK( t, invalid_is( parse_http_url( bad, u ), "bad port in url" ) );
    CHECK( t, invalid_is( parse_http_url( "http://:8080/video", u ), "empty host in url" ) );
    CHECK( t, invalid_is( parse_http_url( "http:///video", u ), "empty host in url" ) );
}
