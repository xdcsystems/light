// CPU affinity and RT priority of the hot threads: a bursty cv-woken worker is not
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
// noexcept: called first thing in the noexcept NPU / reid worker thread bodies, and on the main thread at Pipeline
// startup. A throw from the Log stream (std::ios_base::failure, std::bad_alloc, std::system_error of its mutex) drops
// that log line; the thread keeps running.
void pin_current_thread_to_big_cores( const char* who, const Log& log ) noexcept;

// Pins the calling thread to a specific A76 core (`cpu >= 0`) or the full A76 cluster (`cpu < 0`) and optionally
// switches it to SCHED_FIFO (`rt_prio > 0`). Best-effort and noexcept.
void pin_current_thread_rt( const char* who, int cpu, int rt_prio, const Log& log ) noexcept;

// Locks current and future process pages in RAM (mlockall) to prevent RT page faults.
// Best-effort and noexcept (logs a warning on EPERM/ENOMEM).
void lock_process_memory( const Log& log ) noexcept;
