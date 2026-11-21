#include <exception>
#include <sched.h>

#include "core/log.hpp"
#include "sys/cpu_affinity.hpp"

namespace
{
    enum { kBigCpuFirst = 4, kBigCpuLast = 7 };  // RK3588: Cortex-A76 cluster

    void log_pin_result( const char* who, int cpu, bool pinned, const Log& log )
    {
        if ( !pinned )
        {
            if ( cpu >= 0 )
            {
                log.warn() << "pin: " << who << " -> cpu" << cpu << " failed, scheduler fallback\n";
                return;
            }
            log.warn() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast
                       << " failed, scheduler fallback\n";
            return;
        }
        if ( cpu >= 0 )
        {
            log.info() << "pin: " << who << " -> A76 cpu" << cpu << '\n';
            return;
        }
        log.info() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast << '\n';
    }
}  // namespace

void pin_current_thread_to_big_cores( const char* who, const Log& log ) noexcept
{
    pin_current_thread_rt( who, -1, 0, log );
}

void pin_current_thread_rt( const char* who, int cpu, int rt_prio, const Log& log ) noexcept
{
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    if ( cpu >= 0 )
    {
        CPU_SET( cpu, &cpuset );
    }
    else
    {
        for ( int c = kBigCpuFirst; c <= kBigCpuLast; ++c )
            CPU_SET( c, &cpuset );
    }
    const bool pinned = sched_setaffinity( 0, sizeof( cpuset ), &cpuset ) == 0;

    bool rt_ok = false;
    if ( rt_prio > 0 )
    {
        sched_param sp {};
        sp.sched_priority = rt_prio;
        rt_ok = sched_setscheduler( 0, SCHED_FIFO, &sp ) == 0;
    }

    // Only the Log::Line can throw, and all it throws derives from std::exception (std::ios_base::failure,
    // std::bad_alloc, std::system_error of its mutex lock). The line is dropped; the pin (or the fallback) stands.
    // Anything else would be a bug: noexcept turns it into std::terminate.
    try
    {
        log_pin_result( who, cpu, pinned, log );
        if ( rt_prio > 0 )
        {
            if ( rt_ok )
            {
                log.info() << "rt: " << who << " -> SCHED_FIFO prio " << rt_prio << '\n';
                return;
            }
            log.warn() << "rt: " << who << " -> SCHED_FIFO prio " << rt_prio << " failed, SCHED_OTHER fallback\n";
        }
    }
    catch ( const std::exception& )
    {
    }
}
