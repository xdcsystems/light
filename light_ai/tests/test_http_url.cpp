#include <string>

#include "check.hpp"
#include "core/http_url.hpp"
#include "suites.hpp"

void test_http_url( TestRun& t )
{
    HttpUrl u;
    std::string why;
    CHECK( t, parse_http_url( "http://192.168.1.2:8080/video", u, &why ) );
    CHECK( t, u.host == "192.168.1.2" && u.port == 8080 && u.path == "/video" );
    CHECK( t, parse_http_url( "http://cam.local", u, &why ) );
    CHECK( t, u.host == "cam.local" && u.port == 80 && u.path == "/" );
    CHECK( t, parse_http_url( "http://h:65535/a/b?x=1", u, nullptr ) && u.port == 65535 && u.path == "/a/b?x=1" );
    CHECK( t, !parse_http_url( "https://h/", u, &why ) && why == "url must start with http://" );
    for ( const char* bad : { "http://h:0/", "http://h:65536/", "http://h:/", "http://h:80x/", "http://h:-1/" } )
        CHECK( t, !parse_http_url( bad, u, &why ) && why == "bad port in url" );
    CHECK( t, !parse_http_url( "http://:8080/video", u, &why ) && why == "empty host in url" );
    CHECK( t, !parse_http_url( "http:///video", u, &why ) && why == "empty host in url" );
}
