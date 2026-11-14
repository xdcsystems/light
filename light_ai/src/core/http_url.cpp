#include <cerrno>
#include <cstdlib>
#include <string>

#include "core/http_url.hpp"

namespace
{
    bool fail( std::string* why, const char* text )
    {
        if ( why )
            *why = text;
        return false;
    }
}  // namespace

bool parse_http_url( const std::string& url, HttpUrl& out, std::string* why )
{
    // No named constant for the scheme length on purpose: a function-local
    // static constexpr needs the s_ prefix (naming), a plain constexpr needs
    // lower_case (naming), and dropping static trips
    // static-definition-in-anonymous-namespace. No named form passes all
    // three; magic-number checks are disabled project-wide.
    if ( url.compare( 0, 7, "http://" ) != 0 )
        return fail( why, "url must start with http://" );
    const std::string body = url.substr( 7 );
    const size_t slash = body.find( '/' );
    const std::string hp = ( slash == std::string::npos ) ? body : body.substr( 0, slash );
    out.path = ( slash == std::string::npos ) ? std::string( "/" ) : body.substr( slash );
    const size_t colon = hp.rfind( ':' );
    if ( colon == std::string::npos )
    {
        out.host = hp;
        out.port = 80;
    }
    else
    {
        out.host = hp.substr( 0, colon );
        // strtol with a full parse check (cert-err34-c): no atoi.
        const char* ps = hp.c_str() + colon + 1;
        errno = 0;
        char* end = nullptr;
        const long pv = std::strtol( ps, &end, 10 );
        if ( errno != 0 || end == ps || *end != '\0' || pv < 1 || pv > 65535 )
            return fail( why, "bad port in url" );
        out.port = static_cast<int>( pv );
    }
    if ( out.host.empty() )
        return fail( why, "empty host in url" );
    return true;
}
