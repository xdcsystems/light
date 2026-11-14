#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

#include "check.hpp"
#include "core/packet_queue.hpp"
#include "suites.hpp"

namespace
{
    struct FakePkt
    {
        int seq = -1;  // -1 = no payload
        bool key = false;
        bool alloc = false;
    };

    struct Stats
    {
        int inits = 0;
        int releases = 0;
        int unrefs = 0;  // payloads dropped (not moved on)
        int fail_at = -1;
    };

    struct FakeOps
    {
        bool init( FakePkt& p )
        {
            if ( st->inits == st->fail_at )
                return false;
            ++st->inits;
            p.alloc = true;
            return true;
        }
        void release( FakePkt& p )
        {
            ++st->releases;
            p.alloc = false;
        }
        void unref( FakePkt& p )
        {
            if ( p.seq >= 0 )
                ++st->unrefs;
            p.seq = -1;
        }
        void move( FakePkt& dst, FakePkt& src )
        {
            unref( dst );
            dst.seq = src.seq;
            dst.key = src.key;
            src.seq = -1;
        }
        bool is_key( const FakePkt& p ) const { return p.key; }

    public:
        Stats* st = nullptr;
    };

    using Queue = BasicPacketQueue<FakePkt, FakeOps, 4>;
    using Pop = Queue::Pop;

    FakePkt pkt( int seq, bool key )
    {
        FakePkt p;
        p.seq = seq;
        p.key = key;
        return p;
    }

    void push( Queue& q, int seq, bool key )
    {
        FakePkt p = pkt( seq, key );
        q.push( p );
    }

    void slots( TestRun& t )
    {
        Stats st;
        {
            Queue q( FakeOps { &st } );
            CHECK( t, st.inits == 4 && st.releases == 0 );
        }
        CHECK( t, st.releases == 4 );
        // A failed slot allocation releases what was allocated and throws.
        Stats bad;
        bad.fail_at = 2;
        bool threw = false;
        try
        {
            Queue q( FakeOps { &bad } );
        }
        catch ( const std::runtime_error& )
        {
            threw = true;
        }
        CHECK( t, threw && bad.inits == 2 && bad.releases == 2 );
    }

    void push_pop( TestRun& t )
    {
        Stats st;
        Queue q( FakeOps { &st } );
        // Closed: a push drops the packet.
        FakePkt p = pkt( 1, true );
        CHECK( t, q.push( p ).flushed == 0 && p.seq == -1 && q.depth() == 0 && st.unrefs == 1 );

        q.open( 1 );
        // The key gate: packets before the first IDR are dropped by pop().
        push( q, 2, false );
        push( q, 3, false );
        push( q, 4, true );
        push( q, 5, false );
        CHECK( t, q.depth() == 4 );
        FakePkt out;
        Queue::Popped r = q.pop( out );
        CHECK( t, r.what == Pop::Packet && out.seq == 4 && r.left == 1 );
        CHECK( t, st.unrefs == 3 );  // 1 (closed) + 2 gated
        // After the key frame, P-frames pass.
        r = q.pop( out );
        CHECK( t, r.what == Pop::Packet && out.seq == 5 && r.left == 0 );

        // close() drops the leftovers.
        push( q, 6, true );
        q.close();
        CHECK( t, q.depth() == 0 && st.unrefs == 5 );  // + 4 (out overwritten by the next move) + 6
    }

    void overflow( TestRun& t )
    {
        Stats st;
        Queue q( FakeOps { &st } );
        q.open( 1 );
        push( q, 1, true );
        push( q, 2, false );
        push( q, 3, false );
        push( q, 4, false );
        // Full: the next push flushes all four, re-arms the key gate.
        FakePkt p = pkt( 5, false );
        Queue::Pushed pu = q.push( p );
        CHECK( t, pu.flushed == 4 && pu.drops == 1 && q.depth() == 1 );
        push( q, 6, true );
        FakePkt out;
        Queue::Popped r = q.pop( out );  // 5 is dropped by the re-armed gate
        CHECK( t, r.what == Pop::Packet && out.seq == 6 );
        // A second overflow counts on.
        for ( int s = 7; s <= 10; ++s )
            push( q, s, true );
        p = pkt( 11, true );
        pu = q.push( p );
        CHECK( t, pu.flushed == 4 && pu.drops == 2 );
    }

    void failures( TestRun& t )
    {
        Stats st;
        Queue q( FakeOps { &st } );
        FakePkt out;
        // A fail() while closed is a no-op.
        q.fail( 0 );
        q.open( 1 );
        push( q, 1, true );
        // Another session's fail() is a no-op; this session's ends it, queued packets or not.
        q.fail( 2 );
        q.fail( 0 );
        CHECK( t, q.pop( out ).what == Pop::Packet && out.seq == 1 );
        push( q, 2, false );
        q.fail( 1 );
        CHECK( t, q.pop( out ).what == Pop::Failed );
        CHECK( t, q.pop( out ).what == Pop::Failed );  // sticky until the next open()
        q.close();
        // The stale-error case: a fail() for session 1 that lands after session 2 opened.
        q.open( 2 );
        q.fail( 1 );
        push( q, 3, true );
        CHECK( t, q.pop( out ).what == Pop::Packet && out.seq == 3 );
        // stop() wins over packets and failures.
        push( q, 4, false );
        q.fail( 2 );
        q.stop();
        CHECK( t, q.pop( out ).what == Pop::Stopped );
    }
}  // namespace

void test_packet_queue( TestRun& t )
{
    slots( t );
    push_pop( t );
    overflow( t );
    failures( t );
}

// Threaded (also run under TSan): a producer pushes and fails, a consumer
// runs 2000 short sessions (open / pop / close), a third thread reads the
// depth; at the end stop() wakes a consumer blocked on an empty queue.
void test_packet_queue_threads( TestRun& t )
{
    Stats st;
    BasicPacketQueue<FakePkt, FakeOps, 8> q( FakeOps { &st } );
    std::atomic<uint32_t> epoch { 0 };
    std::atomic<bool> done { false };
    std::atomic<int> first_not_key { 0 };
    std::atomic<int> sessions_failed { 0 };
    std::atomic<int> ready { 0 };
    constexpr int k_sessions = 2000;
    // Start latch: the three threads run their loops at the same time.
    const auto latch = [&ready] {
        ready.fetch_add( 1 );
        while ( ready.load() < 3 )
            std::this_thread::yield();
    };

    std::thread consumer( [&] {
        latch();
        FakePkt out;
        for ( uint32_t s = 1; s <= k_sessions; ++s )
        {
            q.open( s );
            epoch.store( s );
            bool first = true;
            for ( int n = 0; n < 12; ++n )
            {
                const auto r = q.pop( out );
                if ( r.what != BasicPacketQueue<FakePkt, FakeOps, 8>::Pop::Packet )
                {
                    sessions_failed.fetch_add( 1 );
                    break;
                }
                if ( first && !out.key )
                    first_not_key.fetch_add( 1 );
                first = false;
            }
            q.close();
        }
        done.store( true );
    } );

    std::thread producer( [&] {
        latch();
        int seq = 0;
        while ( !done.load() )
        {
            FakePkt p = pkt( seq, seq % 5 == 0 );
            q.push( p );
            const uint32_t e = epoch.load();
            if ( seq % 7 == 0 )
                q.fail( e - 1 );  // stale: never ends a session
            if ( seq % 997 == 0 )
                q.fail( e );  // real: may end the open session
            ++seq;
            std::this_thread::yield();
        }
    } );

    std::atomic<int> depth_out_of_range { 0 };
    std::thread reader( [&] {
        latch();
        while ( !done.load() )
        {
            const int d = q.depth();
            if ( d < 0 || d > 8 )
                depth_out_of_range.fetch_add( 1 );
            std::this_thread::yield();
        }
    } );

    consumer.join();
    producer.join();
    reader.join();
    CHECK( t, first_not_key.load() == 0 );   // every session starts at a key frame
    CHECK( t, depth_out_of_range.load() == 0 );
    CHECK( t, sessions_failed.load() < k_sessions / 2 );  // stale fails never end sessions

    // stop() wakes a consumer blocked on an empty open queue.
    q.open( k_sessions + 1 );
    std::atomic<int> got { -1 };
    std::thread blocked( [&] {
        FakePkt out;
        got.store( static_cast<int>( q.pop( out ).what ) );
    } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    q.stop();
    blocked.join();
    CHECK( t, got.load() == static_cast<int>( BasicPacketQueue<FakePkt, FakeOps, 8>::Pop::Stopped ) );
}
