#include <atomic>
#include <cstdint>
#include <thread>

#include "check.hpp"
#include "core/session_gate.hpp"
#include "suites.hpp"

namespace
{
    SessionGate::Session make( uint32_t epoch )
    {
        SessionGate::Session s;
        s.stream_index = static_cast<int>( epoch % 7 );
        s.frame_step = static_cast<int64_t>( epoch ) * 3;
        s.epoch = epoch;
        s.force_idr = ( epoch % 2 ) == 0;
        return s;
    }

    bool consistent( const SessionGate::Session& s )
    {
        return s.stream_index == static_cast<int>( s.epoch % 7 ) && s.frame_step == static_cast<int64_t>( s.epoch ) * 3
               && s.force_idr == ( ( s.epoch % 2 ) == 0 );
    }
}  // namespace

void test_session_gate( TestRun& t )
{
    SessionGate g;
    int calls = 0;
    // Closed: run() does not call f.
    CHECK( t, !g.active() && g.epoch() == 0 );
    CHECK( t, !g.run( [&]( const SessionGate::Session& ) { ++calls; } ) );
    CHECK( t, calls == 0 );

    // Open: f sees the session as published.
    g.open( make( 4 ) );
    CHECK( t, g.active() && g.epoch() == 4 );
    SessionGate::Session seen;
    CHECK( t, g.run( [&]( const SessionGate::Session& s ) {
        ++calls;
        seen = s;
    } ) );
    CHECK( t, calls == 1 && seen.epoch == 4 && consistent( seen ) && seen.force_idr );

    // Closed again: the epoch stays readable, run() refuses.
    g.close();
    CHECK( t, !g.active() && g.epoch() == 4 );
    CHECK( t, !g.run( [&]( const SessionGate::Session& ) { ++calls; } ) );
    CHECK( t, calls == 1 );

    // The next session replaces the fields.
    g.open( make( 5 ) );
    g.run( [&]( const SessionGate::Session& s ) { seen = s; } );
    CHECK( t, seen.epoch == 5 && consistent( seen ) && !seen.force_idr );
}

// Threaded (also run under TSan): the serve side opens and closes 2000
// sessions while the encoder side runs encodes. Inside run() the session is
// always consistent and never one that close() already returned for.
void test_session_gate_threads( TestRun& t )
{
    SessionGate g;
    constexpr uint32_t k_sessions = 2000;
    std::atomic<uint32_t> closed_upto { 0 };
    std::atomic<bool> done { false };
    std::atomic<int> bad { 0 };
    std::atomic<int> encodes { 0 };

    std::thread serve( [&] {
        for ( uint32_t e = 1; e <= k_sessions; ++e )
        {
            g.open( make( e ) );
            std::this_thread::yield();
            g.close();
            closed_upto.store( e );
        }
        done.store( true );
    } );

    std::thread encoder( [&] {
        uint32_t last = 0;
        while ( !done.load() )
        {
            if ( !g.active() )
            {
                std::this_thread::yield();
                continue;
            }
            const uint32_t snap = g.epoch();
            g.run( [&]( const SessionGate::Session& s ) {
                encodes.fetch_add( 1 );
                if ( !consistent( s ) || s.epoch <= closed_upto.load() || s.epoch < last || s.epoch < snap )
                    bad.fetch_add( 1 );
                last = s.epoch;
            } );
        }
    } );

    serve.join();
    encoder.join();
    CHECK( t, bad.load() == 0 );
    CHECK( t, !g.active() && g.epoch() == k_sessions );
    ( void ) encodes;  // how many encodes overlapped a session depends on the scheduler
}
