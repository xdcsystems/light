// ProcessEnv: process-wide signal disposition, owned by main().
//
// Construct it FIRST in main(), before any thread exists: the signal mask
// is per-thread and inherited at thread creation, so every thread spawned
// afterwards (SignalWaiter, NPU workers, prober, encoder/preview serve
// threads, library-internal threads) starts with SIGINT/SIGTERM blocked.
// Those two signals are then consumed synchronously by SignalWaiter
// (sigtimedwait) -- no asynchronous handler, no async-signal-safety rules,
// no global pointer for a handler to reach the Pipeline through.
//
// SIGPIPE is ignored for the whole session: a write to a closed socket
// (viewer gone, FFmpeg internals) must return EPIPE, not kill the process.
// MSG_NOSIGNAL still covers our own send() calls.
//
// Wake signal (SIGUSR1): the stop flag alone does not end a blocking
// poll() in the frame loop (a stalled camera waits up to 2 s). wake()
// sends SIGUSR1 to one thread; its handler does nothing, it exists only
// so that the blocked call returns EINTR at once -- the frame loop then
// re-checks the stop flag. The handler is installed without SA_RESTART
// for exactly that reason.
//
// The destructor restores the previous mask and actions. It runs last in
// main() -- after the Pipeline and its sockets are gone -- so no teardown
// path can hit a default SIGPIPE.

#pragma once

#include <pthread.h>

#include <csignal>

class ProcessEnv
{
public:
    ProcessEnv();  // throws std::runtime_error if the mask or action cannot be set
    ~ProcessEnv();
    ProcessEnv( const ProcessEnv& ) = delete;
    ProcessEnv& operator=( const ProcessEnv& ) = delete;
    ProcessEnv( ProcessEnv&& ) = delete;
    ProcessEnv& operator=( ProcessEnv&& ) = delete;

    // Interrupts a blocking syscall in `thread` (EINTR); no other effect.
    void wake( pthread_t thread ) const;

private:
    sigset_t old_mask_ {};
    struct sigaction old_pipe_ {};
    struct sigaction old_wake_ {};
};
