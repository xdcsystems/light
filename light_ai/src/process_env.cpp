#include "process_env.hpp"

#include <pthread.h>

#include <csignal>
#include <stdexcept>

namespace
{
    // Body intentionally empty: delivery alone makes a blocked poll()/recv()
    // return EINTR. Touches nothing, so it is async-signal-safe by construction.
    void on_wake( int ) {}
}  // namespace

ProcessEnv::ProcessEnv()
{
    sigset_t stop_set {};
    sigemptyset( &stop_set );
    sigaddset( &stop_set, SIGINT );
    sigaddset( &stop_set, SIGTERM );
    if ( pthread_sigmask( SIG_BLOCK, &stop_set, &old_mask_ ) != 0 )
        throw std::runtime_error( "process: cannot block SIGINT/SIGTERM" );

    struct sigaction ignore {};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): SIG_IGN expands to a C-style
    ignore.sa_handler = SIG_IGN;  // cast inside the glibc system header; the cast is not ours to fix here.
    sigemptyset( &ignore.sa_mask );
    if ( sigaction( SIGPIPE, &ignore, &old_pipe_ ) != 0 )
    {
        pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr );
        throw std::runtime_error( "process: cannot ignore SIGPIPE" );
    }

    struct sigaction wake_act {};
    wake_act.sa_handler = on_wake;  // sa_flags = 0: no SA_RESTART, the interrupted call must return EINTR
    sigemptyset( &wake_act.sa_mask );
    if ( sigaction( SIGUSR1, &wake_act, &old_wake_ ) != 0 )
    {
        sigaction( SIGPIPE, &old_pipe_, nullptr );
        pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr );
        throw std::runtime_error( "process: cannot install the wake handler" );
    }
}

ProcessEnv::~ProcessEnv()
{
    sigaction( SIGUSR1, &old_wake_, nullptr );
    sigaction( SIGPIPE, &old_pipe_, nullptr );
    pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr );
}

void ProcessEnv::wake( pthread_t thread ) const
{
    // Fails only for a thread that no longer exists; nothing to wake then.
    static_cast<void>( pthread_kill( thread, SIGUSR1 ) );
}
