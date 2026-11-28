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

#include <cstddef>
#include <cstdint>

class Log;

enum
{
    kRtStackPrefaultBytes = 128 * 1024
};

// Pins the calling thread to the A76 cluster. `who` names the thread in the log.
// noexcept: called first thing in the noexcept NPU / reid worker thread bodies, and on the main thread at Pipeline
// startup. A throw from the Log stream (std::ios_base::failure, std::bad_alloc, std::system_error of its mutex) drops
// that log line; the thread keeps running.
void pin_current_thread_to_big_cores( const char* who, const Log& log ) noexcept;

// Pins the calling thread to a specific A76 core (`cpu >= 0`) or the full A76 cluster (`cpu < 0`) and optionally
// switches it to SCHED_FIFO (`rt_prio > 0`, clamped to the range the kernel reports; the log names the clamped
// value). Best-effort and noexcept.
void pin_current_thread_rt( const char* who, int cpu, int rt_prio, const Log& log ) noexcept;

// Touches `bytes` of the calling thread's stack in 4096-byte page steps to avoid minor page faults on RT paths.
void prefault_current_thread_stack( std::size_t bytes = kRtStackPrefaultBytes ) noexcept;

// Configures glibc allocator (M_TRIM_THRESHOLD=-1, M_MMAP_MAX=0) before mlockall to prevent heap trim/mmap faults.
void configure_rt_allocator( const Log& log ) noexcept;

// Locks current and future process pages in RAM (mlockall) to prevent RT page faults.
// Best-effort and noexcept (logs a warning on EPERM/ENOMEM).
void lock_process_memory( const Log& log ) noexcept;

// Reads cumulative minor and major page faults for the calling thread via getrusage(RUSAGE_THREAD).
void read_thread_page_faults( uint64_t& minflt, uint64_t& majflt ) noexcept;
