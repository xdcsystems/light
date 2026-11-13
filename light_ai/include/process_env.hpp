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
// The destructor restores the previous mask and SIGPIPE action. It runs
// last in main() -- after the Pipeline and its sockets are gone -- so no
// teardown path can hit a default SIGPIPE.

#pragma once

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

private:
    sigset_t old_mask_ {};
    struct sigaction old_pipe_ {};
};
