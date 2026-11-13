#include "process_env.hpp"

#include <pthread.h>

#include <csignal>
#include <stdexcept>

ProcessEnv::ProcessEnv()
{
    sigset_t stop_set;
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
}

ProcessEnv::~ProcessEnv()
{
    sigaction( SIGPIPE, &old_pipe_, nullptr );
    pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr );
}
