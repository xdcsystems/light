#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <initializer_list>
#include <thread>

#include "check.hpp"
#include "core/stop_flag.hpp"
#include "sys/process_env.hpp"
#include "sys/signal_waiter.hpp"
#include "suites.hpp"

namespace
{
    // Polls the flag for up to 2 s: the waiter takes a pending signal
    // immediately, the margin only absorbs scheduler noise.
    bool wait_for( const StopFlag& stop )
    {
        for ( int i = 0; i < 200 && !stop.requested(); ++i )
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        return stop.requested();
    }

    bool blocked_now( int sig )
    {
        sigset_t cur;
        sigemptyset( &cur );
        pthread_sigmask( SIG_BLOCK, nullptr, &cur );
        return sigismember( &cur, sig ) == 1;
    }

    bool sigpipe_ignored()
    {
        struct sigaction cur {};
        sigaction( SIGPIPE, nullptr, &cur );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): SIG_IGN is a C-style cast in glibc
        return cur.sa_handler == SIG_IGN;
    }

    // Runs `body` in a forked child and returns its wait status. Called
    // while the test process is single-threaded: fork() copies only the
    // calling thread, the child builds its own ProcessEnv and waiter.
    template <typename Body>
    int in_child( Body body )
    {
        const pid_t pid = fork();
        if ( pid == 0 )
        {
            body();
            _exit( 0 );
        }
        int status = 0;
        waitpid( pid, &status, 0 );
        return status;
    }

    bool killed_by( int status, int sig )
    {
        return WIFSIGNALED( status ) && WTERMSIG( status ) == sig;
    }

    bool exited_ok( int status )
    {
        return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
    }

    // Child body: an armed stop ends a blocking poll() on a stalled source that watches the wake fd next to
    // its own: without the wake it would sit out the whole 5 s timeout. Exit code 0 = as expected.
    void wake_fd_ends_blocked_poll()
    {
        const ProcessEnv env;
        StopFlag stop;
        SignalWaiter waiter( env, stop );
        const SignalWaiter::Armed armed( waiter );
        int fds[2] = { -1, -1 };
        if ( stop.wake_fd() < 0 || pipe2( fds, O_CLOEXEC ) != 0 )
            _exit( 4 );
        std::thread sender( [] {
            std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
            kill( getpid(), SIGINT );
        } );
        pollfd pfds[2] = { { fds[0], POLLIN, 0 }, { stop.wake_fd(), POLLIN, 0 } };
        const auto t0 = std::chrono::steady_clock::now();
        const int pr = poll( pfds, 2, 5000 );
        const auto waited = std::chrono::steady_clock::now() - t0;
        sender.join();
        const bool ok = pr > 0 && ( pfds[1].revents & POLLIN ) != 0 && stop.requested() && waited < std::chrono::seconds( 2 );
        close( fds[0] );
        close( fds[1] );
        if ( !ok )
            _exit( 5 );
    }

    // Child body: the signal is taken BEFORE poll() starts; the wake fd is level-triggered, so nothing is lost.
    void wake_fd_keeps_early_stop()
    {
        const ProcessEnv env;
        StopFlag stop;
        SignalWaiter waiter( env, stop );
        const SignalWaiter::Armed armed( waiter );
        int fds[2] = { -1, -1 };
        if ( stop.wake_fd() < 0 || pipe2( fds, O_CLOEXEC ) != 0 )
            _exit( 4 );
        kill( getpid(), SIGINT );
        if ( !wait_for( stop ) )
            _exit( 3 );
        // request() sets the flag first and writes the eventfd right after: poll() may start in between.
        pollfd pfds[2] = { { fds[0], POLLIN, 0 }, { stop.wake_fd(), POLLIN, 0 } };
        const auto t0 = std::chrono::steady_clock::now();
        const int pr = poll( pfds, 2, 5000 );
        const auto waited = std::chrono::steady_clock::now() - t0;
        const bool ok = pr > 0 && ( pfds[1].revents & POLLIN ) != 0 && waited < std::chrono::milliseconds( 500 );
        close( fds[0] );
        close( fds[1] );
        if ( !ok )
            _exit( 5 );
    }

    // No signal: the destructor wakes the thread out of its untimed poll() at once.
    void fast_destructor( TestRun& t )
    {
        const ProcessEnv env;
        StopFlag stop;
        std::chrono::steady_clock::time_point t0;
        {
            const SignalWaiter waiter( env, stop );
            std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );  // the thread is in poll() by now
            t0 = std::chrono::steady_clock::now();
        }
        const auto el = std::chrono::steady_clock::now() - t0;
        // A woken destructor takes microseconds; the bound leaves room for a loaded board or a TSan host.
        CHECK( t, el < std::chrono::milliseconds( 250 ) );
        CHECK( t, !stop.requested() && stop.wake_fd() == -1 );
    }
}  // namespace

void test_signals( TestRun& t )
{
    // Unarmed (Pipeline ctor/dtor): the signal keeps its default action.
    for ( const int sig : { SIGINT, SIGTERM } )
    {
        const int status = in_child( [sig] {
            const ProcessEnv env;
            StopFlag stop;
            const SignalWaiter waiter( env, stop );
            kill( getpid(), sig );
            std::this_thread::sleep_for( std::chrono::seconds( 2 ) );  // the child must die before this ends
        } );
        CHECK( t, killed_by( status, sig ) );
    }
    // Armed, then disarmed: back to the default action after the scope.
    {
        const int status = in_child( [] {
            const ProcessEnv env;
            StopFlag stop;
            SignalWaiter waiter( env, stop );
            {
                const SignalWaiter::Armed armed( waiter );
            }
            kill( getpid(), SIGINT );
            std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
        } );
        CHECK( t, killed_by( status, SIGINT ) );
    }
    // Armed: the signal only sets the flag, the process lives on and exits normally.
    {
        const int status = in_child( [] {
            const ProcessEnv env;
            StopFlag stop;
            SignalWaiter waiter( env, stop );
            const SignalWaiter::Armed armed( waiter );
            kill( getpid(), SIGTERM );
            if ( !wait_for( stop ) )
                _exit( 3 );
            kill( getpid(), SIGINT );  // a second Ctrl-C while armed is harmless too
            std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
        } );
        CHECK( t, WIFEXITED( status ) && WEXITSTATUS( status ) == 0 );
    }

    // Armed stop and a poll() that watches the wake fd: signal during the wait, and before it.
    CHECK( t, exited_ok( in_child( [] { wake_fd_ends_blocked_poll(); } ) ) );
    CHECK( t, exited_ok( in_child( [] { wake_fd_keeps_early_stop(); } ) ) );
    fast_destructor( t );

    // In-process: ProcessEnv sets and restores the disposition.
    CHECK( t, !blocked_now( SIGINT ) && !blocked_now( SIGTERM ) && !sigpipe_ignored() );
    {
        const ProcessEnv env;
        CHECK( t, blocked_now( SIGINT ) && blocked_now( SIGTERM ) && sigpipe_ignored() );

        // A write to a pipe with no reader returns EPIPE instead of killing the process.
        int fds[2] = { -1, -1 };
        CHECK( t, pipe2( fds, O_CLOEXEC ) == 0 );
        close( fds[0] );
        const char byte = 'x';
        errno = 0;
        const ssize_t n = write( fds[1], &byte, 1 );
        CHECK( t, n == -1 && errno == EPIPE );
        close( fds[1] );

        // Armed waiter in this process: SIGINT reaches the flag.
        StopFlag stop;
        SignalWaiter waiter( env, stop );
        const SignalWaiter::Armed armed( waiter );
        CHECK( t, !stop.requested() );
        kill( getpid(), SIGINT );
        CHECK( t, wait_for( stop ) );
    }
    CHECK( t, !blocked_now( SIGINT ) && !blocked_now( SIGTERM ) && !sigpipe_ignored() );
}
