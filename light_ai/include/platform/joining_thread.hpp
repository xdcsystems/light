// JoiningThread: RAII wrapper over std::thread that always joins on destruction
// or before starting a new thread. C++17 stdlib only, no virtuals, no heap.

#pragma once

#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include "core/non_copyable.hpp"

class JoiningThread : private NonCopyable<JoiningThread>
{
public:
    JoiningThread() noexcept = default;

    template <typename Fn, typename... Args,
              std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, JoiningThread>, int> = 0>
    explicit JoiningThread( Fn&& fn, Args&&... args )
      : th_( std::forward<Fn>( fn ), std::forward<Args>( args )... )
    {
    }

    ~JoiningThread() { join(); }

    JoiningThread( JoiningThread&& other ) noexcept : th_( std::move( other.th_ ) ) {}

    JoiningThread& operator=( JoiningThread&& other ) noexcept
    {
        if ( this != &other )
        {
            join();
            th_ = std::move( other.th_ );
        }
        return *this;
    }

    // Joins any currently running thread, then starts a new one.
    template <typename Fn, typename... Args>
    void start( Fn&& fn, Args&&... args )
    {
        join();
        th_ = std::thread( std::forward<Fn>( fn ), std::forward<Args>( args )... );
    }

    // Joins the thread if joinable; idempotent and noexcept-safe for dtor / move-assign.
    void join() noexcept
    {
        try
        {
            if ( th_.joinable() )
                th_.join();
        }
        catch ( const std::system_error& )
        {
        }
    }

    bool joinable() const noexcept { return th_.joinable(); }

private:
    std::thread th_;
};
