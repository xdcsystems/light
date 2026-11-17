#include <exception>
#include <iostream>

#include "config/cli.hpp"  // CliOptions: argv + config-file reading/validation
#include "core/log.hpp"
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
// the Log first (the parse already reports through it), then the parsed
// options, then -- inside the try block -- env (before any thread), the
// flag, the waiter and the Pipeline; the Armed scope ends before the
// Pipeline is destroyed.
int main( int argc, char** argv )
{
    // Diagnostics: info/debug -> stdout, warnings/errors -> stderr.
    // Declared first, outside the try block: CliOptions uses Info threshold
    // (so option/INI errors on stderr and "config: loaded" print before
    // --log-level is known); afterwards log applies cfg.log_level.
    const Log cli_log( std::cout, std::cerr );

    CliOptions cli( cli_log );
    switch ( cli.parse( argc, argv ) )
    {
        case CliOptions::ParseStatus::Help:
            cli.print_usage( std::cout );  // the usage is the program's output, not a diagnostic
            return 0;
        case CliOptions::ParseStatus::Error:
            return 2;
        case CliOptions::ParseStatus::Run:
            break;
    }

    const Log log( std::cout, std::cerr, cli.config().log_level );

    try
    {
        const ProcessEnv env;
        StopFlag stop;
        SignalWaiter waiter( env, stop );
        Pipeline p( cli.config(), stop, log );
        const SignalWaiter::Armed armed( waiter );
        const Status rst = p.run();
        return rst.ok() ? 0 : 1;  // a runtime fatal -> 1; a clean end (even zero frames) -> 0
    }
    catch ( const std::exception& e )
    {
        log.error() << "Fatal: " << e.what() << '\n';
        return 3;  // startup failure; the CLI rejections above keep their 2
    }
}
