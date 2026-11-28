// SignalWaiter: turns SIGINT/SIGTERM into a StopFlag request.
//
// A dedicated thread blocks in poll() on two descriptors without a timeout:
// a signalfd for the two signals and a private eventfd that only the
// destructor writes. ProcessEnv must already have blocked the signals, so
// they stay pending until this thread reads them. Being an ordinary thread,
// it may lock, log and touch any object -- none of the async-signal-safety
// limits of a handler.
//
// Two modes, switched by the Armed scope:
// - armed   (Pipeline::run in progress): the signal calls StopFlag::request(),
//   which sets the flag and writes the wake eventfd (StopFlag::wake_fd()).
//   The source waits (SourceSet::wait_any, CameraV4L2, BufferedSocket) poll
//   that fd next to their own: it is level-triggered and never read, so a
//   request that lands before poll() starts is not lost. No signal is sent to
//   another thread, so no blocking call (an SDK ioctl) gets EINTR. run()
//   leaves the frame loop through the normal drain/summary path.
// - unarmed (Pipeline ctor/dtor, before/after run): the signal terminates
//   the process with its default action, exactly as before this class --
//   Ctrl-C during model loading or teardown does not wait.
//
// Declare it after ProcessEnv and the StopFlag, before the Pipeline: it
// must run while the Pipeline is constructed (unarmed Ctrl-C) and both
// must outlive it (the Pipeline holds copies of wake_fd()). The destructor
// writes the private eventfd, so the thread leaves poll() at once.
// The wake eventfd is best-effort (if it cannot be created, wake_fd() stays
// -1 and the waits end by their own timeouts); the signalfd and the private
// eventfd are required: the ctor throws std::runtime_error without them.

#pragma once

#include <atomic>

#include "core/non_copyable.hpp"
#include "core/stop_flag.hpp"
#include "platform/joining_thread.hpp"
#include "sys/process_env.hpp"

class SignalWaiter : private NonMovable<SignalWaiter>
{
public:
    SignalWaiter( const ProcessEnv& env, StopFlag& stop );
    ~SignalWaiter();

    // RAII scope of the armed mode: armed for its lifetime, unarmed after,
    // including unwinding from an exception.
    class Armed : private NonMovable<Armed>
    {
    public:
        explicit Armed( SignalWaiter& waiter );
        ~Armed();

    private:
        SignalWaiter& waiter_;
    };

private:
    struct WakeFd : private NonMovable<WakeFd>
    {
        explicit WakeFd( StopFlag& s ) noexcept;
        ~WakeFd();
        StopFlag& stop;
        int fd = -1;
    };

    // Closes the descriptor it owns.
    struct OwnedFd : private NonMovable<OwnedFd>
    {
        explicit OwnedFd( int owned ) noexcept : fd( owned ) {}
        ~OwnedFd();
        const int fd;
    };

    void loop() noexcept;
    void on_signal( int sig );

private:
    StopFlag& stop_;
    WakeFd wake_;
    OwnedFd sig_fd_;   // signalfd for SIGINT/SIGTERM
    OwnedFd quit_fd_;  // eventfd, written by the destructor only
    std::atomic<bool> armed_ { false };
    std::atomic<bool> quit_ { false };
    JoiningThread thread_;  // last: started once the descriptors and flags above exist
};
