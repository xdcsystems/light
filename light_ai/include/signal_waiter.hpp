// SignalWaiter: turns SIGINT/SIGTERM into a StopFlag request.
//
// A dedicated thread waits for the two signals synchronously (sigtimedwait);
// ProcessEnv must already have blocked them, so they stay pending until
// this thread takes them. Being an ordinary thread, it may lock, log and
// touch any object -- none of the async-signal-safety limits of a handler.
//
// Two modes, switched by the Armed scope:
// - armed   (Pipeline::run in progress): the signal sets the StopFlag;
//   run() leaves the frame loop through the normal drain/summary path.
// - unarmed (Pipeline ctor/dtor, before/after run): the signal terminates
//   the process with its default action, exactly as before this class --
//   Ctrl-C during model loading or teardown does not wait.
//
// Declare it after ProcessEnv and the StopFlag, before the Pipeline: it
// must run while the Pipeline is constructed (unarmed Ctrl-C) and the flag
// must outlive it. The destructor stops the thread within one wait tick.

#pragma once

#include <atomic>
#include <thread>

#include "core/stop_flag.hpp"

class SignalWaiter
{
public:
    explicit SignalWaiter( StopFlag& stop );
    ~SignalWaiter();
    SignalWaiter( const SignalWaiter& ) = delete;
    SignalWaiter& operator=( const SignalWaiter& ) = delete;
    SignalWaiter( SignalWaiter&& ) = delete;
    SignalWaiter& operator=( SignalWaiter&& ) = delete;

    // RAII scope of the armed mode: armed for its lifetime, unarmed after,
    // including unwinding from an exception.
    class Armed
    {
    public:
        explicit Armed( SignalWaiter& waiter );
        ~Armed();
        Armed( const Armed& ) = delete;
        Armed& operator=( const Armed& ) = delete;
        Armed( Armed&& ) = delete;
        Armed& operator=( Armed&& ) = delete;

    private:
        SignalWaiter& waiter_;
    };

private:
    void loop();
    void on_signal( int sig );

    StopFlag& stop_;
    std::atomic<bool> armed_ { false };
    std::atomic<bool> quit_ { false };
    std::thread thread_;  // last: started once the flags above exist
};
