// PiMutex & PiCondVar: RAII wrappers over pthread_mutex_t (PTHREAD_PRIO_INHERIT)
// and pthread_cond_t (CLOCK_MONOTONIC) to prevent priority inversion on RT paths.
// Zero heap allocations, C++17, noexcept lock/unlock/notify.

#pragma once

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <mutex>
#include <pthread.h>

#include "core/log.hpp"

namespace light_ai
{
namespace platform
{
namespace detail
{
template <class Rep, class Period>
inline timespec monotonic_deadline( const std::chrono::duration<Rep, Period>& rel ) noexcept
{
    timespec ts {};
    ::clock_gettime( CLOCK_MONOTONIC, &ts );
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>( rel ).count();
    if ( ns <= 0 )
        return ts;
    const long sec_ns = 1000000000L;
    ts.tv_sec += static_cast<time_t>( ns / sec_ns );
    ts.tv_nsec += static_cast<long>( ns % sec_ns );
    if ( ts.tv_nsec >= sec_ns )
    {
        ++ts.tv_sec;
        ts.tv_nsec -= sec_ns;
    }
    return ts;
}
}  // namespace detail

class PiMutex
{
public:
    explicit PiMutex( const Log* log = nullptr ) noexcept
    {
        pthread_mutexattr_t attr {};
        ::pthread_mutexattr_init( &attr );
        const int set_rc = ::pthread_mutexattr_setprotocol( &attr, PTHREAD_PRIO_INHERIT );
        int init_rc = ( set_rc == 0 ) ? ::pthread_mutex_init( &mx_, &attr ) : set_rc;
        if ( init_rc == 0 )
        {
            pi_active_ = true;
            ::pthread_mutexattr_destroy( &attr );
            return;
        }

        // Fallback when kernel/container does not support PTHREAD_PRIO_INHERIT.
        ::pthread_mutexattr_setprotocol( &attr, PTHREAD_PRIO_NONE );
        init_rc = ::pthread_mutex_init( &mx_, &attr );
        ::pthread_mutexattr_destroy( &attr );
        if ( log != nullptr )
        {
            try
            {
                log->warn() << "pi_mutex: PTHREAD_PRIO_INHERIT unsupported (rc=" << init_rc
                            << "), fallback to PTHREAD_PRIO_NONE\n";
            }
            catch ( const std::exception& )
            {
            }
        }
    }

    ~PiMutex() { ::pthread_mutex_destroy( &mx_ ); }

    PiMutex( const PiMutex& ) = delete;
    PiMutex& operator=( const PiMutex& ) = delete;
    PiMutex( PiMutex&& ) = delete;
    PiMutex& operator=( PiMutex&& ) = delete;

    void lock() noexcept { ::pthread_mutex_lock( &mx_ ); }
    bool try_lock() noexcept { return ::pthread_mutex_trylock( &mx_ ) == 0; }
    void unlock() noexcept { ::pthread_mutex_unlock( &mx_ ); }

    pthread_mutex_t* native_handle() noexcept { return &mx_; }
    bool prio_inherit_enabled() const noexcept { return pi_active_; }

private:
    pthread_mutex_t mx_ {};
    bool pi_active_ = false;
};

class PiCondVar
{
public:
    PiCondVar() noexcept
    {
        pthread_condattr_t attr {};
        ::pthread_condattr_init( &attr );
        ::pthread_condattr_setclock( &attr, CLOCK_MONOTONIC );
        ::pthread_cond_init( &cv_, &attr );
        ::pthread_condattr_destroy( &attr );
    }

    ~PiCondVar() { ::pthread_cond_destroy( &cv_ ); }

    PiCondVar( const PiCondVar& ) = delete;
    PiCondVar& operator=( const PiCondVar& ) = delete;
    PiCondVar( PiCondVar&& ) = delete;
    PiCondVar& operator=( PiCondVar&& ) = delete;

    void notify_one() noexcept { ::pthread_cond_signal( &cv_ ); }
    void notify_all() noexcept { ::pthread_cond_broadcast( &cv_ ); }

    void wait( std::unique_lock<PiMutex>& lk ) noexcept { ::pthread_cond_wait( &cv_, lk.mutex()->native_handle() ); }

    template <class Pred>
    void wait( std::unique_lock<PiMutex>& lk, Pred pred ) noexcept( noexcept( pred() ) )
    {
        while ( !pred() )
            wait( lk );
    }

    template <class Rep, class Period>
    std::cv_status wait_for( std::unique_lock<PiMutex>& lk, const std::chrono::duration<Rep, Period>& rel ) noexcept
    {
        const timespec ts = detail::monotonic_deadline( rel );
        const int rc = ::pthread_cond_timedwait( &cv_, lk.mutex()->native_handle(), &ts );
        return rc == ETIMEDOUT ? std::cv_status::timeout : std::cv_status::no_timeout;
    }

    template <class Rep, class Period, class Pred>
    bool wait_for( std::unique_lock<PiMutex>& lk, const std::chrono::duration<Rep, Period>& rel,
                   Pred pred ) noexcept( noexcept( pred() ) )
    {
        const timespec ts = detail::monotonic_deadline( rel );
        while ( !pred() )
        {
            if ( ::pthread_cond_timedwait( &cv_, lk.mutex()->native_handle(), &ts ) == ETIMEDOUT )
                return pred();
        }
        return true;
    }

private:
    pthread_cond_t cv_ {};
};

}  // namespace platform
}  // namespace light_ai
