#include <exception>
#include <iostream>

#include "config/cli.hpp"  // CliOptions: argv + config-file reading/validation
#include "core/stop_flag.hpp"
#include "pipeline.hpp"
#include "process_env.hpp"
#include "signal_waiter.hpp"

// Graceful shutdown without globals: ProcessEnv blocks SIGINT/SIGTERM
// before any thread exists; SignalWaiter takes them on its own thread.
// While Pipeline::run() is in progress (Armed scope) a signal sets the
// StopFlag, wakes this thread out of a blocking poll(), and run() exits
// through the normal path (ring drain, TOTAL summary, encoder flush,
// camera STREAMOFF/teardown). Outside that scope
// -- model loading in the ctor, teardown in the dtor -- a signal keeps its
// default action and ends the process at once.
//
// Declaration order is the lifetime contract (destruction runs in reverse):
// env first (before any thread), then the flag, then the waiter, then the
// Pipeline; the Armed scope ends before the Pipeline is destroyed.
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

    try
    {
        const ProcessEnv env;
        StopFlag stop;
        SignalWaiter waiter( env, stop );
        Pipeline p( cli.config(), stop );
        const SignalWaiter::Armed armed( waiter );
        const int rc = p.run();
        return rc > 0 ? 0 : 1;
    }
    catch ( const std::exception& e )
    {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 2;
    }
}
