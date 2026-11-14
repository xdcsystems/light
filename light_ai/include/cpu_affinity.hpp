// CPU affinity of the hot threads (2026-11-12 audit follow-up; supersedes
// the old "the scheduler places hot threads on A76" note in CMakeLists):
// a bursty cv-woken worker is not guaranteed to stay on the big cluster
// under CFS/EAS. RK3588 stock DT: A76 = cpu 4-7.
//
// Best-effort: any error keeps the scheduler default and logs a warning --
// a run on an alien board (or a test on a PC) must not die here.
// A child thread inherits its creator's mask, so a thread that must keep
// the scheduler default (the hot-attach prober) is spawned before its
// creator pins itself.
//
// SDK-free.

#pragma once

class Log;

// Pins the calling thread to the A76 cluster. `who` names the thread in the log.
void pin_current_thread_to_big_cores( const char* who, const Log& log );
