#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

#include "sys/signal_waiter.hpp"

namespace
{
    sigset_t stop_signals()
    {
        sigset_t set {};
        sigemptyset( &set );
        sigaddset( &set, SIGINT );
        sigaddset( &set, SIGTERM );
        return set;
    }

    // Startup only (the SignalWaiter ctor): both throw when the kernel refuses.
    int open_signal_fd()
    {
        const sigset_t set = stop_signals();
        const int fd = ::signalfd( -1, &set, SFD_NONBLOCK | SFD_CLOEXEC );
        if ( fd < 0 )
            throw std::runtime_error( "signal: signalfd failed" );
        return fd;
    }

    int open_quit_fd()
    {
        const int fd = ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC );
        if ( fd < 0 )
            throw std::runtime_error( "signal: eventfd failed" );
        return fd;
    }

    void signal_wake_fd( const void* ctx ) noexcept
    {
        if ( ctx == nullptr )
            return;
        const int fd = *static_cast<const int*>( ctx );
        if ( fd >= 0 )
        {
            const uint64_t one = 1;
            static_cast<void>( ::write( fd, &one, sizeof( one ) ) );
        }
    }

    // Default action of `sig` for the whole process: restore SIG_DFL,
    // unblock the signal in this thread and re-raise it -- the process ends
    // "killed by SIGINT/SIGTERM", the same status the shell saw before.
    [[noreturn]] void terminate_by( int sig )
    {
        struct sigaction dfl {};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): SIG_DFL expands to a C-style
        dfl.sa_handler = SIG_DFL;  // cast inside the glibc system header; the cast is not ours to fix here.
        sigemptyset( &dfl.sa_mask );
        sigaction( sig, &dfl, nullptr );

        sigset_t one {};
        sigemptyset( &one );
        sigaddset( &one, sig );
        pthread_sigmask( SIG_UNBLOCK, &one, nullptr );
        // raise() can only fail for an invalid signal number; either way the
        // _Exit below ends the process with the shell's 128 + signo convention.
        static_cast<void>( raise( sig ) );
        std::_Exit( 128 + sig );  // not reached: the default action terminates
    }
}  // namespace

SignalWaiter::WakeFd::WakeFd( StopFlag& s ) noexcept : stop( s ), fd( ::eventfd( 0, EFD_NONBLOCK | EFD_CLOEXEC ) )
{
    if ( fd >= 0 )
        stop.attach_wake( fd, &fd, &signal_wake_fd );
}

SignalWaiter::WakeFd::~WakeFd()
{
    stop.attach_wake( -1, nullptr, nullptr );
    if ( fd >= 0 )
        ::close( fd );
}

SignalWaiter::OwnedFd::~OwnedFd()
{
    if ( fd >= 0 )
        ::close( fd );
}

// env: proof at compile time that ProcessEnv has blocked SIGINT/SIGTERM and outlives the waiter.
SignalWaiter::SignalWaiter( const ProcessEnv& env, StopFlag& stop )
  : stop_( stop ), wake_( stop ), sig_fd_( open_signal_fd() ), quit_fd_( open_quit_fd() ), thread_( &SignalWaiter::loop, this )
{
    static_cast<void>( env );
}

SignalWaiter::~SignalWaiter()
{
    quit_.store( true, std::memory_order_release );
    const uint64_t one = 1;
    static_cast<void>( ::write( quit_fd_.fd, &one, sizeof( one ) ) );
    thread_.join();
}

void SignalWaiter::loop() noexcept
{
    constexpr short k_bad = POLLERR | POLLHUP | POLLNVAL;
    pollfd pfds[2] = { { sig_fd_.fd, POLLIN, 0 }, { quit_fd_.fd, POLLIN, 0 } };
    while ( !quit_.load( std::memory_order_acquire ) )
    {
        const int rc = ::poll( pfds, 2, -1 );  // no timeout: no periodic wakeups
        if ( rc < 0 && errno == EINTR )
            continue;
        if ( rc < 0 )
        {
            // Without this thread no later signal can be seen (they stay blocked): stop the run instead.
            stop_.request();
            break;
        }
        if ( ( pfds[1].revents & ( POLLIN | k_bad ) ) != 0 || quit_.load( std::memory_order_acquire ) )
            break;
        if ( ( pfds[0].revents & POLLIN ) != 0 )
        {
            // One signal per poll() round: the fd stays readable while another one is pending.
            signalfd_siginfo si {};
            const ssize_t n = ::read( sig_fd_.fd, &si, sizeof( si ) );
            if ( n == static_cast<ssize_t>( sizeof( si ) ) )
                on_signal( static_cast<int>( si.ssi_signo ) );
        }
        if ( ( pfds[0].revents & k_bad ) != 0 )
            break;
    }
}

void SignalWaiter::on_signal( int sig )
{
    if ( armed_.load( std::memory_order_acquire ) )
    {
        stop_.request();  // the flag, then the level-triggered wake eventfd
        return;
    }
    terminate_by( sig );
}

SignalWaiter::Armed::Armed( SignalWaiter& waiter ) : waiter_( waiter )
{
    waiter_.armed_.store( true, std::memory_order_release );
}

SignalWaiter::Armed::~Armed()
{
    waiter_.armed_.store( false, std::memory_order_release );
}
