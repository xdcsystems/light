#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <malloc.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "core/log.hpp"
#include "core/status.hpp"
#include "sys/cpu_affinity.hpp"

namespace
{
    enum
    {
        kBigCpuFirst = 4,
        kBigCpuLast = 7,  // RK3588: Cortex-A76 cluster
        kStackPageBytes = 4096,
        kMaxPrefaultBytes = 512 * 1024
    };

#if defined( __GNUC__ ) || defined( __clang__ )
    __attribute__( ( noinline ) )
#endif
    void touch_stack_pages( std::size_t remaining ) noexcept
    {
        volatile unsigned char page[kStackPageBytes];
        page[0] = 0;
        page[sizeof( page ) - 1] = 0;
        if ( remaining > sizeof( page ) )
            touch_stack_pages( remaining - sizeof( page ) );
        static_cast<void>( page[0] );
    }

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
    bool valid_cpu = true;
    if ( cpu >= 0 )
    {
        valid_cpu = cpu < CPU_SETSIZE;
        if ( valid_cpu )
            CPU_SET( cpu, &cpuset );
    }
    else
    {
        for ( int c = kBigCpuFirst; c <= kBigCpuLast; ++c )
            CPU_SET( c, &cpuset );
    }
    const bool pinned = valid_cpu && sched_setaffinity( 0, sizeof( cpuset ), &cpuset ) == 0;

    bool rt_ok = false;
    int rt_err = 0;
    int eff_prio = rt_prio;  // clamped to the SCHED_FIFO range of the running kernel
    if ( rt_prio > 0 )
    {
        const int pmin = sched_get_priority_min( SCHED_FIFO );
        const int pmax = sched_get_priority_max( SCHED_FIFO );
        if ( pmin >= 0 && pmax >= pmin )
            eff_prio = std::clamp( rt_prio, pmin, pmax );
        sched_param sp {};
        sp.sched_priority = eff_prio;
        rt_ok = sched_setscheduler( 0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp ) == 0;
        if ( !rt_ok )
            rt_err = errno;
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
                log.info() << "rt: " << who << " -> SCHED_FIFO prio " << eff_prio << '\n';
                return;
            }
            log.warn() << "rt: " << who << " -> SCHED_FIFO prio " << eff_prio
                       << " failed (" << ErrnoText { rt_err } << "), SCHED_OTHER fallback\n";
        }
    }
    catch ( const std::exception& )
    {
    }
}

void lock_process_memory( const Log& log ) noexcept
{
    const bool locked = ::mlockall( MCL_CURRENT | MCL_FUTURE ) == 0;
    const int err = errno;
    try
    {
        if ( locked )
        {
            log.info() << "rt: mlockall(MCL_CURRENT|MCL_FUTURE) OK\n";
            return;
        }
        log.warn() << "rt: mlockall failed (" << ErrnoText { err } << "), continuing unlocked\n";
    }
    catch ( const std::exception& )
    {
    }
}


void prefault_current_thread_stack( std::size_t bytes ) noexcept
{
    const std::size_t clamped = std::min<std::size_t>( bytes, kMaxPrefaultBytes );
    if ( clamped > 0 )
        touch_stack_pages( clamped );
}

void configure_rt_allocator( const Log& log ) noexcept
{
    const int trim_ok = ::mallopt( M_TRIM_THRESHOLD, -1 );
    const int mmap_ok = ::mallopt( M_MMAP_MAX, 0 );
    try
    {
        if ( trim_ok == 0 || mmap_ok == 0 )
        {
            log.warn() << "mallopt: trim_ok=" << trim_ok << " mmap_ok=" << mmap_ok << " (continuing)\n";
            return;
        }
        log.info() << "mallopt: M_TRIM_THRESHOLD=-1, M_MMAP_MAX=0 OK\n";
    }
    catch ( const std::exception& )
    {
    }
}

void read_thread_page_faults( uint64_t& minflt, uint64_t& majflt ) noexcept
{
    struct rusage ru {};
    if ( ::getrusage( RUSAGE_THREAD, &ru ) == 0 )
    {
        minflt = ru.ru_minflt > 0 ? static_cast<uint64_t>( ru.ru_minflt ) : 0U;
        majflt = ru.ru_majflt > 0 ? static_cast<uint64_t>( ru.ru_majflt ) : 0U;
        return;
    }
    minflt = 0;
    majflt = 0;
}
