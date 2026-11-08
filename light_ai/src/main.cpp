#include <atomic>
#include <csignal>
#include <iostream>

#include "cli.hpp"      // CliOptions: argv + config-file reading/validation
#include "pipeline.hpp"

// Graceful shutdown (P1-6): SIGINT/SIGTERM only set the stop flag;
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
    explicit SignalGuard( Pipeline* p ) { g_pipe.store( p, std::memory_order_release ); }
    ~SignalGuard() { g_pipe.store( nullptr, std::memory_order_release ); }
    SignalGuard( const SignalGuard& ) = delete;
    SignalGuard& operator=( const SignalGuard& ) = delete;
};

int main( int argc, char** argv )
{
    // Parameter reading/validation moved to CliOptions (cli.cpp,
    // 2026-11-08): the option table, the INI config-file layer, the
    // cross-field checks and --help all live there; main() stays the
    // composition root (parse -> Pipeline -> run -> exit code).
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

    try
    {
        Pipeline p( cli.config() );
        std::signal( SIGINT, on_stop_signal );
        std::signal( SIGTERM, on_stop_signal );

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
