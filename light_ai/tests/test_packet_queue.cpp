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
        bool init( FakePkt& p ) const
        {
            if ( st->inits == st->fail_at )
                return false;
            ++st->inits;
            p.alloc = true;
            return true;
        }
        void release( FakePkt& p ) const
        {
            ++st->releases;
            p.alloc = false;
        }
        void unref( FakePkt& p ) const
        {
            if ( p.seq >= 0 )
                ++st->unrefs;
            p.seq = -1;
        }
        void move( FakePkt& dst, FakePkt& src ) const
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
            const Queue q( FakeOps { &st } );
            CHECK( t, st.inits == 4 && st.releases == 0 );
        }
        CHECK( t, st.releases == 4 );
        // A failed slot allocation releases what was allocated and throws.
        Stats bad;
        bad.fail_at = 2;
        bool threw = false;
        try
        {
            const Queue q( FakeOps { &bad } );
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
        const Queue::Popped r = q.pop( out );  // 5 is dropped by the re-armed gate
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

    void age_limit( TestRun& t )
    {
        Stats st;
        Queue q( FakeOps { &st }, 100000 );  // 100 ms age limit
        q.open( 1 );
        FakePkt p1 = pkt( 1, true );
        FakePkt p2 = pkt( 2, false );
        FakePkt p3 = pkt( 3, false );
        FakePkt p4 = pkt( 4, true );

        CHECK( t, q.push( p1, 1000 ).flushed == 0 );
        CHECK( t, q.push( p2, 50000 ).flushed == 0 );  // age 49 ms <= 100 ms
        // Age 101 ms > 100 ms: flushes p1+p2, re-arms key gate, keeps p3.
        const Queue::Pushed pu = q.push( p3, 102000 );
        CHECK( t, pu.flushed == 2 && pu.drops == 1 && st.unrefs == 2 );
        CHECK( t, q.depth() == 1 );
        // p3 is non-key and dropped by re-armed key gate; p4 (key) is popped.
        CHECK( t, q.push( p4, 110000 ).flushed == 0 );
        FakePkt out;
        const Queue::Popped pop = q.pop( out );
        CHECK( t, pop.what == Pop::Packet && out.seq == 4 && pop.left == 0 );
        CHECK( t, st.unrefs == 3 );

        // max_age_us <= 0 disables age-based flush.
        Queue q_no_age( FakeOps { &st }, 0 );
        q_no_age.open( 1 );
        FakePkt a = pkt( 10, true );
        FakePkt b = pkt( 11, false );
        CHECK( t, q_no_age.push( a, 0 ).flushed == 0 );
        CHECK( t, q_no_age.push( b, 10000000 ).flushed == 0 );
        CHECK( t, q_no_age.depth() == 2 );

        // Wait-key non-key packet aged > max_age_us is drained silently on keyframe push.
        Stats st_wait;
        Queue q_wait( FakeOps { &st_wait }, 100000 );
        q_wait.open( 1 );
        FakePkt w1 = pkt( 1, true );
        FakePkt w2 = pkt( 2, false );
        FakePkt w3 = pkt( 3, false );
        FakePkt w5 = pkt( 5, true );
        CHECK( t, q_wait.push( w1, 1000 ).flushed == 0 );
        CHECK( t, q_wait.push( w2, 50000 ).flushed == 0 );
        const Queue::Pushed pu_fl = q_wait.push( w3, 102000 );
        CHECK( t, pu_fl.flushed == 2 && pu_fl.drops == 1 && q_wait.depth() == 1 );
        const Queue::Pushed pu_key = q_wait.push( w5, 450000 );
        CHECK( t, pu_key.flushed == 0 && pu_key.drops == 1 && q_wait.depth() == 1 );
        FakePkt wout;
        const Queue::Popped wpop = q_wait.pop( wout );
        CHECK( t, wpop.what == Pop::Packet && wout.seq == 5 && wpop.left == 0 );
        CHECK( t, st_wait.unrefs == 3 );

        // 3-arg push() invokes on_flush only when flushed > 0 and before moving src.
        Stats st_cb;
        Queue q_cb( FakeOps { &st_cb }, 100000 );
        q_cb.open( 1 );
        int cb_calls = 0;
        int cb_flushed = 0;
        const auto no_flush_cb = [&]( FakePkt& /*p*/, int /*fl*/ ) noexcept { ++cb_calls; };
        FakePkt c1 = pkt( 1, true );
        FakePkt c2 = pkt( 2, false );
        CHECK( t, q_cb.push( c1, 1000, no_flush_cb ).flushed == 0 && cb_calls == 0 );
        CHECK( t, q_cb.push( c2, 50000, no_flush_cb ).flushed == 0 && cb_calls == 0 );
        FakePkt c3 = pkt( 3, true );
        const Queue::Pushed pu_cb = q_cb.push( c3, 102000, [&]( FakePkt& p, int flushed ) noexcept {
            ++cb_calls;
            cb_flushed = flushed;
            if ( p.seq == 3 )
                p.seq = 300;
        } );
        CHECK( t, pu_cb.flushed == 2 && cb_calls == 1 && cb_flushed == 2 );
        FakePkt cout;
        CHECK( t, q_cb.pop( cout ).what == Pop::Packet && cout.seq == 300 );
        // Draining aged wait_key_ non-key packet leaves flushed == 0 so on_flush is not called.
        FakePkt c4 = pkt( 4, false );
        FakePkt c5 = pkt( 5, false );
        FakePkt c6 = pkt( 6, true );
        CHECK( t, q_cb.push( c4, 150000 ).flushed == 0 );
        CHECK( t, q_cb.push( c5, 251000 ).flushed == 1 );  // flushes c4, leaves c5 with wait_key_
        cb_calls = 0;
        CHECK( t, q_cb.push( c6, 400000, no_flush_cb ).flushed == 0 && cb_calls == 0 );
    }

    void backlog_stats( TestRun& t )
    {
        // backlog() reports live depth and oldest packet age, skipping wait_key_ dead weight.
        Stats st_bl;
        Queue q_bl( FakeOps { &st_bl }, 300000 );
        q_bl.open( 1 );
        const Queue::Backlog b0 = q_bl.backlog( 10000 );
        CHECK( t, b0.depth == 0 && b0.head_age_us == 0 );
        FakePkt b_p1 = pkt( 1, false );  // gated non-key while wait_key_ == true
        CHECK( t, q_bl.push( b_p1, 10000 ).flushed == 0 );
        const Queue::Backlog b1 = q_bl.backlog( 260000 );
        CHECK( t, b1.depth == 0 && b1.head_age_us == 0 );
        FakePkt b_k1 = pkt( 2, true );
        FakePkt b_p2 = pkt( 3, false );
        CHECK( t, q_bl.push( b_k1, 200000 ).flushed == 0 );
        CHECK( t, q_bl.push( b_p2, 240000 ).flushed == 0 );
        const Queue::Backlog b2 = q_bl.backlog( 320000 );
        CHECK( t, b2.depth == 2 && b2.head_age_us == 120000 );
        // Non-monotonic now_us clamps age to 0.
        const Queue::Backlog b3 = q_bl.backlog( 150000 );
        CHECK( t, b3.depth == 2 && b3.head_age_us == 0 );
    }
}  // namespace

void test_packet_queue( TestRun& t )
{
    slots( t );
    push_pop( t );
    overflow( t );
    failures( t );
    age_limit( t );
    backlog_stats( t );
}

namespace
{
    using RaceQueue = BasicPacketQueue<FakePkt, FakeOps, 8>;

    // The shared state of test_packet_queue_threads, one method per thread.
    class QueueRace
    {
    public:
        enum { k_sessions = 2000 };

        explicit QueueRace( Stats& st ) : q( FakeOps { &st } ) {}

        // Start latch: the three threads run their loops at the same time.
        void latch()
        {
            ready.fetch_add( 1 );
            while ( ready.load() < 3 )
                std::this_thread::yield();
        }

        // Consumer: k_sessions short sessions (open / pop up to 12 / close).
        void consume()
        {
            latch();
            FakePkt out;
            for ( uint32_t s = 1; s <= k_sessions; ++s )
            {
                q.open( s );
                epoch.store( s );
                pop_session( out );
                q.close();
            }
            done.store( true );
        }

        // Producer: pushes, a stale fail every 7th packet, a real one every 997th.
        void produce()
        {
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
        }

        // Reader: depth and backlog stay within valid bounds while the others run.
        void read_depth()
        {
            latch();
            while ( !done.load() )
            {
                const int d = q.depth();
                const RaceQueue::Backlog bl = q.backlog();
                if ( d < 0 || d > 8 || bl.depth < 0 || bl.depth > 8 || bl.head_age_us < 0 )
                    depth_out_of_range.fetch_add( 1 );
                std::this_thread::yield();
            }
        }

    private:
        void pop_session( FakePkt& out )
        {
            for ( int n = 0; n < 12; ++n )
            {
                if ( q.pop( out ).what != RaceQueue::Pop::Packet )
                {
                    sessions_failed.fetch_add( 1 );
                    return;
                }
                if ( n == 0 && !out.key )
                    first_not_key.fetch_add( 1 );
            }
        }

    public:
        RaceQueue q;
        std::atomic<uint32_t> epoch { 0 };
        std::atomic<bool> done { false };
        std::atomic<int> first_not_key { 0 };
        std::atomic<int> sessions_failed { 0 };
        std::atomic<int> depth_out_of_range { 0 };

    private:
        std::atomic<int> ready { 0 };
    };
}  // namespace

// Threaded (also run under TSan): a producer pushes and fails, a consumer
// runs 2000 short sessions (open / pop / close), a third thread reads the
// depth; at the end stop() wakes a consumer blocked on an empty queue.
void test_packet_queue_threads( TestRun& t )
{
    Stats st;
    QueueRace race( st );
    std::thread consumer( [&race] { race.consume(); } );
    std::thread producer( [&race] { race.produce(); } );
    std::thread reader( [&race] { race.read_depth(); } );
    consumer.join();
    producer.join();
    reader.join();
    CHECK( t, race.first_not_key.load() == 0 );   // every session starts at a key frame
    CHECK( t, race.depth_out_of_range.load() == 0 );
    CHECK( t, race.sessions_failed.load() < static_cast<int>( QueueRace::k_sessions ) / 2 );  // stale fails never end sessions

    // stop() wakes a consumer blocked on an empty open queue.
    RaceQueue& q = race.q;
    q.open( QueueRace::k_sessions + 1 );
    std::atomic<int> got { -1 };
    std::thread blocked( [&] {
        FakePkt out;
        got.store( static_cast<int>( q.pop( out ).what ) );
    } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    q.stop();
    blocked.join();
    CHECK( t, got.load() == static_cast<int>( RaceQueue::Pop::Stopped ) );
}
