#include <atomic>
#include <csignal>
#include <iostream>

#include "config/cli.hpp"  // CliOptions: argv + config-file reading/validation
#include "pipeline.hpp"

// Graceful shutdown: SIGINT/SIGTERM only set the stop flag;
// Pipeline::run() picks it up on the next frame and exits through the
// normal path (ring drain, TOTAL summary, encoder flush, camera
// STREAMOFF/teardown). The handler dereferences an atomic store only
// (de facto async-signal-safe); g_pipe is owned by SignalGuard (declared
// after the Pipeline, so it is destroyed BEFORE it) and cleared on every
// exit path -- including unwinding from an exception in run() -- thus it
// never points at a destroyed or dying Pipeline.
std::atomic<Pipeline*> g_pipe { nullptr };
void on_stop_signal( int )
{
    Pipeline* p = g_pipe.load( std::memory_order_acquire );
    if ( p )
        p->request_stop();
}

// RAII guard for g_pipe: it must never outlive the Pipeline. Set in
// the ctor, cleared in the dtor; since destruction runs in reverse
// declaration order, the guard (declared after p) dies BEFORE p --
// so a signal arriving during unwinding (exception from run()) or
// inside ~Pipeline() can no longer touch a dead or dying object.
// Trivially constructible/destructible: no heap, no statics.
struct SignalGuard
{
    explicit SignalGuard( Pipeline* p )
    {
        g_pipe.store( p, std::memory_order_release );
        std::signal( SIGINT, on_stop_signal );
        std::signal( SIGTERM, on_stop_signal );

        // SIGPIPE is ignored process-wide in main(), BEFORE the Pipeline ctor:
        // the serve threads (NetEncoder, NetPreview) already run while this
        // guard is created -- see the comment at the top of main().
    }
    ~SignalGuard()
    {
        std::signal( SIGINT, SIG_DFL );
        std::signal( SIGTERM, SIG_DFL );
        g_pipe.store( nullptr, std::memory_order_release );
    }
    SignalGuard( const SignalGuard& ) = delete;
    SignalGuard& operator=( const SignalGuard& ) = delete;
};

int main( int argc, char** argv )
{
    CliOptions cli;
    switch ( cli.parse( argc, argv ) )
    {
        case CliOptions::ParseStatus::Help:
            return 0;
        case CliOptions::ParseStatus::Error:
            return 2;
        case CliOptions::ParseStatus::Run:
            break;
    }

    // Network daemon: writes to closed sockets/pipes must not kill the process.
    // MSG_NOSIGNAL covers our own send calls; SIG_IGN covers the rest (FFmpeg
    // internals, any fd that turns out to be a broken pipe). Set BEFORE the
    // Pipeline ctor: the NetEncoder/NetPreview serve threads start inside the
    // ctor, before SignalGuard -- a viewer connecting in that window must not
    // hit an unprotected send(). Deliberately NOT restored to SIG_DFL anywhere:
    // during unwinding the teardown path (FFmpeg flush) may still write to
    // sockets, and dying with SIGPIPE there would skip cleanup entirely.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): SIG_IGN expands to a C-style
    std::signal( SIGPIPE, SIG_IGN ); // cast inside the glibc system header; the cast is not ours to fix here.

    try
    {
        Pipeline p( cli.config() );
        SignalGuard guard( &p );
        const int rc = p.run();
        return rc > 0 ? 0 : 1;
    }
    catch ( const std::exception& e )
    {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 2;
    }
}
