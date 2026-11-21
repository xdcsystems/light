// JoiningThread: RAII wrapper over std::thread that always joins on destruction
// or before starting a new thread. C++17 stdlib only, no virtuals, no heap.

#pragma once

#include <thread>
#include <type_traits>
#include <utility>

class JoiningThread
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

    JoiningThread( const JoiningThread& ) = delete;
    JoiningThread& operator=( const JoiningThread& ) = delete;

    JoiningThread( JoiningThread&& other ) noexcept : th_( std::move( other.th_ ) ) {}

    JoiningThread& operator=( JoiningThread&& other )
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

    // Joins the thread if joinable; idempotent.
    void join()
    {
        if ( th_.joinable() )
            th_.join();
    }

    bool joinable() const noexcept { return th_.joinable(); }

private:
    std::thread th_;
};
