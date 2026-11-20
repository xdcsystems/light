// CPU affinity of the hot threads: a bursty cv-woken worker is not
// guaranteed to stay on the big cluster under CFS/EAS.
// RK3588 stock DT: A76 = cpu 4-7.
//
// Best-effort: any error keeps the scheduler default and logs a warning --
// a run on an alien board (or a test on a PC) must not die here.
// A child thread inherits its creator's mask, so a thread that must keep
// the scheduler default (the hot-attach prober) is spawned before its
// creator pins itself.

#pragma once

class Log;

// Pins the calling thread to the A76 cluster. `who` names the thread in the log.
// noexcept: called first thing in the noexcept NPU / reid worker thread bodies. A throw from the Log stream
// (std::ios_base::failure, std::bad_alloc, std::system_error of its mutex) drops that log line; the thread keeps running.
void pin_current_thread_to_big_cores( const char* who, const Log& log ) noexcept;
