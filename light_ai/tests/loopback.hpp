// Loopback TCP helpers of the host tests: a listener on an ephemeral port of
// 127.0.0.1 and a blocking write of a whole buffer (BufferedSocket and
// NetMjpegSource tests: the real socket code, no fakes).

#pragma once

#include <cstddef>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace loopback
{

// A loopback listener on an ephemeral port; accept() hands out the server end.
// A failed socket / bind / listen / getsockname leaves port() at 0 and no socket:
// the test's connect to port 0 then fails visibly instead of hanging on a half-set listener.
class Listener
{
public:
    Listener()
    {
        fd_ = ::socket( AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0 );
        if ( fd_ < 0 )
            return;
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
        a.sin_port = 0;
        socklen_t len = sizeof( a );
        if ( ::bind( fd_, reinterpret_cast<sockaddr*>( &a ), sizeof( a ) ) != 0 || ::listen( fd_, 4 ) != 0 ||
             ::getsockname( fd_, reinterpret_cast<sockaddr*>( &a ), &len ) != 0 )
        {
            close();
            return;
        }
        port_ = ntohs( a.sin_port );
    }
    ~Listener()
    {
        if ( fd_ >= 0 )
            ::close( fd_ );
    }
    Listener( const Listener& ) = delete;
    Listener& operator=( const Listener& ) = delete;

    int port() const { return port_; }
    int accept() const { return ::accept4( fd_, nullptr, nullptr, SOCK_CLOEXEC ); }
    void close()
    {
        ::close( fd_ );
        fd_ = -1;
    }

private:
    int fd_ = -1;
    int port_ = 0;
};

inline void write_all( int fd, const std::string& s )
{
    size_t off = 0;
    while ( off < s.size() )
    {
        const ssize_t w = ::send( fd, s.data() + off, s.size() - off, MSG_NOSIGNAL );
        if ( w <= 0 )
            return;
        off += static_cast<size_t>( w );
    }
}

// Reads up to and including the empty line of a request head (the GET).
inline std::string read_request( int fd )
{
    std::string req;
    char c = 0;
    while ( req.size() < 4096 && ::recv( fd, &c, 1, 0 ) == 1 )
    {
        req.push_back( c );
        if ( req.size() >= 4 && req.compare( req.size() - 4, 4, "\r\n\r\n" ) == 0 )
            break;
    }
    return req;
}

}  // namespace loopback
