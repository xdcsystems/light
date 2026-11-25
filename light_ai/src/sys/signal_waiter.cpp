#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>

#include "sys/signal_waiter.hpp"

namespace
{
    // Wait tick: how often the thread re-checks quit_. Bounds the
    // destructor's join latency; signals themselves arrive immediately.
    enum : long { kTickNs = 100L * 1000L * 1000L };  // 100 ms; long: the type of timespec::tv_nsec

    sigset_t stop_signals()
    {
        sigset_t set {};
        sigemptyset( &set );
        sigaddset( &set, SIGINT );
        sigaddset( &set, SIGTERM );
        return set;
    }

    // Default action of `sig` for the whole process: restore SIG_DFL,
    // unblock the signal in this thread and re-raise it -- the process ends
    // "killed by SIGINT/SIGTERM", the same status the shell saw before.
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

SignalWaiter::SignalWaiter( const ProcessEnv& env, StopFlag& stop )
  : env_( env ), stop_( stop ), wake_( stop ), owner_( pthread_self() ), thread_( &SignalWaiter::loop, this )
{
}

SignalWaiter::~SignalWaiter()
{
    quit_.store( true );
    thread_.join();
}

void SignalWaiter::loop() noexcept
{
    const sigset_t set = stop_signals();
    const timespec tick { 0, kTickNs };
    while ( !quit_.load() )
    {
        // > 0: a signal was taken; -1: the tick elapsed (EAGAIN) or EINTR --
        // either way re-check quit_ and wait again.
        const int sig = sigtimedwait( &set, nullptr, &tick );
        if ( sig > 0 )
            on_signal( sig );
    }
}

void SignalWaiter::on_signal( int sig )
{
    if ( armed_.load() )
    {
        // Flag first, wake second: the woken loop must already see the request.
        stop_.request();
        env_.wake( owner_ );
        return;
    }
    terminate_by( sig );
}

SignalWaiter::Armed::Armed( SignalWaiter& waiter ) : waiter_( waiter )
{
    waiter_.armed_.store( true );
}

SignalWaiter::Armed::~Armed()
{
    waiter_.armed_.store( false );
}
