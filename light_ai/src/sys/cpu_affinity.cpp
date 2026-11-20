#include <exception>
#include <sched.h>

#include "core/log.hpp"
#include "sys/cpu_affinity.hpp"

namespace
{
    enum { kBigCpuFirst = 4, kBigCpuLast = 7 };  // RK3588: Cortex-A76 cluster
}  // namespace

void pin_current_thread_to_big_cores( const char* who, const Log& log ) noexcept
{
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    for ( int c = kBigCpuFirst; c <= kBigCpuLast; ++c )
        CPU_SET( c, &cpuset );
    const bool pinned = sched_setaffinity( 0, sizeof( cpuset ), &cpuset ) == 0;
    // Only the Log::Line can throw, and all it throws derives from std::exception (std::ios_base::failure,
    // std::bad_alloc, std::system_error of its mutex lock). The line is dropped; the pin (or the fallback) stands.
    // Anything else would be a bug: noexcept turns it into std::terminate.
    try
    {
        if ( !pinned )
        {
            log.warn() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast
                       << " failed, scheduler fallback\n";
            return;
        }
        log.info() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast << '\n';
    }
    catch ( const std::exception& )
    {
    }
}
