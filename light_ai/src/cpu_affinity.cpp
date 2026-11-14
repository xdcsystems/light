#include "cpu_affinity.hpp"

#include <sched.h>

#include "core/log.hpp"

namespace
{
    enum { kBigCpuFirst = 4, kBigCpuLast = 7 };  // RK3588: Cortex-A76 cluster
}  // namespace

void pin_current_thread_to_big_cores( const char* who, const Log& log )
{
    cpu_set_t cpuset;
    CPU_ZERO( &cpuset );
    for ( int c = kBigCpuFirst; c <= kBigCpuLast; ++c )
        CPU_SET( c, &cpuset );
    if ( sched_setaffinity( 0, sizeof( cpuset ), &cpuset ) != 0 )
    {
        log.warn() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast
                   << " failed, scheduler fallback\n";
        return;
    }
    log.info() << "pin: " << who << " -> A76 cpu" << kBigCpuFirst << '-' << kBigCpuLast << '\n';
}
