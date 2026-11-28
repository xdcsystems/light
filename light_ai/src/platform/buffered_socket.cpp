#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/buffered_socket.hpp"

namespace
{
    // A failed connect() / SO_ERROR: nobody there is Absent (the prober retries).
    enum
    {
        kConnectSliceMs = 100
    };

    Status connect_error( int e )
    {
        const bool absent = e == ECONNREFUSED || e == EHOSTUNREACH || e == ENETUNREACH || e == EHOSTDOWN;
        return errno_status( absent ? Errc::Absent : Errc::Failed, "connect", e );
    }

    constexpr short kWakeEvents = POLLIN | POLLERR | POLLHUP | POLLNVAL;

    // One poll() of poll_with_wake():
    enum class Polled : uint8_t
    {
        Ready,    // out: the events of fd
        Timeout,
        Wake,     // the wake fd is readable or broken; it wins over fd
        Intr,     // EINTR
        Error     // errno says why
    };

    // poll() on fd and, if wake_fd >= 0, on the level-triggered wake_fd, which is never read.
    Polled poll_with_wake( int fd, short events, int wake_fd, int timeout_ms, pollfd& out ) noexcept
    {
        pollfd pfds[2] = { { fd, events, 0 }, { wake_fd, POLLIN, 0 } };
        const nfds_t cnt = wake_fd >= 0 ? 2 : 1;
        const int pr = ::poll( pfds, cnt, timeout_ms );
        if ( pr < 0 )
            return errno == EINTR ? Polled::Intr : Polled::Error;
        if ( pr == 0 )
            return Polled::Timeout;
        if ( cnt == 2 && ( pfds[1].revents & kWakeEvents ) != 0 )
            return Polled::Wake;
        out = pfds[0];
        return Polled::Ready;
    }

    // A numeric IPv4 host: resolved in place, no DNS. Not numeric: false.
    bool resolve_numeric( const std::string& host, const std::string& port_str, addrinfo*& res ) noexcept
    {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST;
        return ::getaddrinfo( host.c_str(), port_str.c_str(), &hints, &res ) == 0 && res != nullptr;
    }
}  // namespace

BufferedSocket::~BufferedSocket()
{
    close();
    if ( !lookup_.pending )
        return;
    // EAI_CANCELED / EAI_ALLDONE: glibc is done with the request. EAI_NOTCANCELED: it still reads and writes
    // lookup_ (cb, hints, both strings), which lives in this object (no heap, no mutable globals: CONTRIBUTING),
    // so returning now would be a use-after-free: wait until gai_error() leaves EAI_INPROGRESS, however long the
    // resolver takes (no deadline, cancel ignored).
    if ( ::gai_cancel( &lookup_.cb ) == EAI_NOTCANCELED )
        static_cast<void>( await_lookup( {}, false ) );
    reap_lookup();
}

bool BufferedSocket::cancelled() const noexcept
{
    return ( stop_ != nullptr && stop_->load( std::memory_order_acquire ) ) ||
           ( prober_quit_ != nullptr && prober_quit_->load( std::memory_order_acquire ) );
}

bool BufferedSocket::wake_signalled() const noexcept
{
    if ( wake_fd_ < 0 )
        return false;
    pollfd p { wake_fd_, POLLIN, 0 };
    return ::poll( &p, 1, 0 ) > 0 && ( p.revents & kWakeEvents ) != 0;
}

Status BufferedSocket::wait_connect( std::chrono::steady_clock::time_point deadline ) noexcept
{
    using clk = std::chrono::steady_clock;
    while ( !is_cancelled() )
    {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
        if ( rem <= 0 )
            return make_status( Errc::Timeout, "connect: timeout" );
        const int slice = static_cast<int>( std::min<long long>( rem, kConnectSliceMs ) );
        pollfd p {};
        const Polled pr = poll_with_wake( fd_, POLLOUT, wake_fd_, slice, p );
        if ( pr == Polled::Timeout || pr == Polled::Intr )
            continue;
        if ( pr == Polled::Wake )
            break;
        if ( pr == Polled::Error )
            return errno_status( Errc::Failed, "connect: poll", errno );
        int so_err = 0;
        socklen_t slen = sizeof( so_err );
        if ( ::getsockopt( fd_, SOL_SOCKET, SO_ERROR, &so_err, &slen ) != 0 )
            return errno_status( Errc::Failed, "connect: getsockopt", errno );
        return so_err == 0 ? Status {} : connect_error( so_err );
    }
    return make_status( Errc::Timeout, "connect: cancelled" );
}

Status BufferedSocket::wait_writable( std::chrono::steady_clock::time_point deadline ) const noexcept
{
    using clk = std::chrono::steady_clock;
    while ( !is_cancelled() )
    {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
        if ( rem <= 0 )
            return make_status( Errc::Timeout, "send: timeout" );
        const int slice = static_cast<int>( std::min<long long>( rem, kConnectSliceMs ) );
        pollfd p {};
        const Polled pr = poll_with_wake( fd_, POLLOUT, wake_fd_, slice, p );
        if ( pr == Polled::Timeout || pr == Polled::Intr )
            continue;
        if ( pr == Polled::Wake )
            break;
        if ( pr == Polled::Error )
            return errno_status( Errc::Lost, "send: poll", errno );
        if ( ( p.revents & POLLNVAL ) != 0 )
            return make_status( Errc::Lost, "send: bad fd" );
        if ( ( p.revents & ( POLLERR | POLLHUP ) ) != 0 && ( p.revents & POLLOUT ) == 0 )
            return make_status( Errc::Lost, "send: peer closed" );
        return {};
    }
    return make_status( Errc::Timeout, "send: cancelled" );
}

int BufferedSocket::await_lookup( std::chrono::steady_clock::time_point deadline, bool bounded ) noexcept
{
    using clk = std::chrono::steady_clock;
    const gaicb* const list[1] = { &lookup_.cb };
    int e = ::gai_error( &lookup_.cb );
    while ( e == EAI_INPROGRESS && ( !bounded || !is_cancelled() ) )
    {
        long long slice = kConnectSliceMs;
        if ( bounded )
        {
            const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
            if ( rem <= 0 )
                break;
            slice = std::min<long long>( rem, kConnectSliceMs );
        }
        const timespec ts { static_cast<time_t>( slice / 1000 ), static_cast<long>( ( slice % 1000 ) * 1000000 ) };
        ::gai_suspend( list, 1, &ts );  // 0, EAI_AGAIN (slice over), EAI_INTR: gai_error() decides
        e = ::gai_error( &lookup_.cb );
    }
    return e;
}

void BufferedSocket::reap_lookup() noexcept
{
    if ( lookup_.cb.ar_result != nullptr )
        freeaddrinfo( lookup_.cb.ar_result );
    lookup_.cb.ar_result = nullptr;
    lookup_.pending = false;
}

Status BufferedSocket::resolve( const std::string& host, const std::string& port_str, addrinfo*& res )
{
    if ( resolve_numeric( host, port_str, res ) )
        return {};
    if ( host.empty() )
        return make_status( Errc::Absent, "addr resolution failed" );
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::milliseconds( kConnectMs );
    // An earlier lookup glibc could not cancel still owns lookup_: wait for it
    // first (same bound), never reuse the request while glibc writes to it.
    if ( lookup_.pending )
    {
        if ( await_lookup( deadline, true ) == EAI_INPROGRESS )
            return make_status( Errc::Timeout, is_cancelled() ? "addr resolution: cancelled" : "addr resolution: timeout" );
        reap_lookup();
    }
    lookup_.name = host;
    lookup_.service = port_str;
    lookup_.hints = addrinfo {};
    lookup_.hints.ai_family = AF_INET;
    lookup_.hints.ai_socktype = SOCK_STREAM;
    lookup_.cb = gaicb {};
    lookup_.cb.ar_name = lookup_.name.c_str();
    lookup_.cb.ar_service = lookup_.service.c_str();
    lookup_.cb.ar_request = &lookup_.hints;
    // No scheduling guard around the call: libanl creates its workers with default attributes, so they inherit the
    // caller's policy, except that threads of a SCHED_RESET_ON_FORK real-time caller start as SCHED_OTHER. The only
    // real-time switch of this program, pin_current_thread_rt(), sets SCHED_FIFO | SCHED_RESET_ON_FORK: keep the flag.
    gaicb* list[1] = { &lookup_.cb };
    if ( ::getaddrinfo_a( GAI_NOWAIT, list, 1, nullptr ) != 0 )
        return make_status( Errc::Absent, "addr resolution failed" );
    lookup_.pending = true;
    const int e = await_lookup( deadline, true );
    if ( e == EAI_INPROGRESS )
    {
        // EAI_CANCELED: dequeued before it ran; EAI_ALLDONE: finished meanwhile;
        // EAI_NOTCANCELED: glibc still runs it -- lookup_ stays pending.
        if ( ::gai_cancel( &lookup_.cb ) != EAI_NOTCANCELED )
            reap_lookup();
        return make_status( Errc::Timeout, is_cancelled() ? "addr resolution: cancelled" : "addr resolution: timeout" );
    }
    if ( e == 0 && lookup_.cb.ar_result != nullptr )
    {
        res = lookup_.cb.ar_result;  // the caller frees it
        lookup_.cb.ar_result = nullptr;
    }
    reap_lookup();
    return res != nullptr ? Status {} : make_status( Errc::Absent, "addr resolution failed" );
}

Status BufferedSocket::connect_one( const addrinfo& ai, std::chrono::steady_clock::time_point deadline )
{
    // SOCK_CLOEXEC: this fd must not leak into a fork()/exec() (this app
    // never forks, but a library or deploy wrapper might). The flag belongs
    // to the socket() type argument -- putting SOCK_CLOEXEC into
    // hints.ai_socktype makes getaddrinfo fail with EAI_SOCKTYPE, killing
    // the connection outright.
    fd_ = ::socket( ai.ai_family, ai.ai_socktype | SOCK_CLOEXEC, ai.ai_protocol );
    if ( fd_ < 0 )
        return errno_status( Errc::Failed, "socket", errno );  // nothing was opened
    // Non-blocking connect up to the deadline, then back to blocking I/O.
    const int fl = ::fcntl( fd_, F_GETFL, 0 );
    if ( fl < 0 || ::fcntl( fd_, F_SETFL, fl | O_NONBLOCK ) != 0 )
    {
        const int e = errno;
        close();
        return errno_status( Errc::Failed, "connect: fcntl", e );
    }
    const int cr = ::connect( fd_, ai.ai_addr, ai.ai_addrlen );
    const int ce = errno;
    if ( cr != 0 && ce != EINPROGRESS )
    {
        close();
        return connect_error( ce );
    }
    if ( cr != 0 )
    {
        if ( Status wst = wait_connect( deadline ); !wst.ok() )
        {
            close();
            return wst;
        }
    }
    if ( ::fcntl( fd_, F_SETFL, fl ) != 0 )  // a socket left non-blocking would break the poll + recv contract
    {
        const int e = errno;
        close();
        return errno_status( Errc::Failed, "connect: fcntl", e );
    }
    int nd = 1;
    ::setsockopt( fd_, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof( nd ) );
    return {};
}

Status BufferedSocket::connect( const std::string& host, int port )
{
    close();
    if ( is_cancelled() )
        return make_status( Errc::Timeout, "connect: cancelled" );
    const std::string port_str = std::to_string( port );
    addrinfo* res = nullptr;
    if ( Status rst = resolve( host, port_str, res ); !rst.ok() )
        return rst;
    // One deadline for every address of the lookup.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( kConnectMs );
    Status last_err = make_status( Errc::Absent, "addr resolution failed" );
    for ( const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next )
    {
        if ( is_cancelled() )
        {
            freeaddrinfo( res );
            return make_status( Errc::Timeout, "connect: cancelled" );
        }
        const Status st = connect_one( *ai, deadline );
        if ( st.ok() || st.code() == Errc::Timeout )
        {
            freeaddrinfo( res );
            return st;
        }
        last_err = st;
    }
    freeaddrinfo( res );
    return last_err;
}

Status BufferedSocket::send_all( const std::string& bytes ) const
{
    if ( fd_ < 0 )
        return make_status( Errc::Failed, "send: not connected" );
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( kSendMs );
    size_t off = 0;
    while ( off < bytes.size() )
    {
        if ( Status wst = wait_writable( deadline ); !wst.ok() )
            return wst;
        // MSG_DONTWAIT: a peer that stopped reading (rwnd 0, half-open TCP) must not
        // hold send() until tcp_retries2 gives up; the bounded wait is the poll above.
        // MSG_NOSIGNAL: a mid-send disconnect must not raise SIGPIPE; the
        // flag keeps this call self-contained even if the global ignore is
        // ever removed.
        const ssize_t w = ::send( fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL | MSG_DONTWAIT );
        if ( w < 0 && ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ) )
            continue;
        if ( w <= 0 )
        {
            // w == 0 cannot happen for a nonzero buffer on a stream
            // socket, but errno is only set on failure: reading it after a
            // zero return would report a stale value from an unrelated call.
            return w < 0 ? errno_status( Errc::Lost, "send", errno ) : make_status( Errc::Lost, "send: peer closed, 0 bytes sent" );
        }
        off += static_cast<size_t>( w );
    }
    return {};
}

BufferedSocket::Fill BufferedSocket::fill( int timeout_ms )
{
    if ( fd_ < 0 )
        return Fill::Closed;
    uint8_t* const dst = win_.tail();  // compacts the unconsumed bytes to the front
    if ( win_.room() == 0 )
        return Fill::Full;
    pollfd p {};
    const Polled pr = poll_with_wake( fd_, POLLIN, wake_fd_, timeout_ms >= 0 ? timeout_ms : 0, p );
    if ( pr == Polled::Intr || pr == Polled::Wake )
        return Fill::Interrupted;
    if ( pr == Polled::Error )
        return Fill::Closed;
    if ( pr == Polled::Timeout )
        return Fill::Timeout;
    if ( ( p.revents & POLLNVAL ) != 0 )
        return Fill::Closed;
    if ( ( p.revents & ( POLLERR | POLLHUP ) ) != 0 && ( p.revents & POLLIN ) == 0 )
        return Fill::Closed;
    // MSG_DONTWAIT: a spurious POLLIN must never block the RT pipeline thread in recv().
    const ssize_t n = ::recv( fd_, dst, win_.room(), MSG_DONTWAIT );
    if ( n < 0 )
    {
        if ( errno == EINTR )
            return Fill::Interrupted;
        if ( errno == EAGAIN || errno == EWOULDBLOCK )
            return Fill::Timeout;
        return Fill::Closed;
    }
    if ( n == 0 )
        return Fill::Closed;
    win_.commit( static_cast<size_t>( n ) );
    return Fill::Data;
}

BufferedSocket::Fill BufferedSocket::fill_until( int total_ms, int slice_ms )
{
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::milliseconds( total_ms );
    const int step_ms = slice_ms > 0 ? slice_ms : 200;
    while ( !is_cancelled() )
    {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>( deadline - clk::now() ).count();
        if ( rem <= 0 )
            return Fill::Timeout;
        const Fill r = fill( static_cast<int>( std::min<long long>( rem, step_ms ) ) );
        if ( r == Fill::Timeout )
            continue;
        if ( r == Fill::Interrupted )
        {
            if ( is_cancelled() )
                return Fill::Interrupted;
            continue;
        }
        return r;
    }
    return Fill::Interrupted;
}

void BufferedSocket::close()
{
    if ( fd_ >= 0 )
    {
        ::close( fd_ );
        fd_ = -1;
    }
    win_.clear();
}
