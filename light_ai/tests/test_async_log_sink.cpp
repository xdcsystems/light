// AsyncLogSink (include/platform/log_sink.hpp) under ThreadSanitizer:
// 8 concurrent writer threads + background drainer thread, 512-slot ring
// overflow (dropped_count() > 0), >kMaxLineBytes truncation, LogSinkFn path,
// and clean drain on destruction.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "platform/log_sink.hpp"
#include "suites.hpp"

namespace
{
    std::size_t count_occurrences( const std::string& haystack, const std::string& needle )
    {
        std::size_t count = 0;
        std::size_t pos = 0;
        while ( ( pos = haystack.find( needle, pos ) ) != std::string::npos )
        {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    std::string make_padded_line( int id, int iter )
    {
        std::string s = "p|" + std::to_string( id ) + ":" + std::to_string( iter ) + "|";
        s.append( 200, 'a' );
        s.push_back( '\n' );
        return s;
    }

    void writer_worker( light_ai::platform::AsyncLogSink& sink, const Log& log, std::atomic<int>& ready,
                        int writers, int lines_per_writer, int id,
                        std::atomic<uint64_t>& accepted_push, std::atomic<uint64_t>& rejected_push )
    {
        ready.fetch_add( 1, std::memory_order_acq_rel );
        while ( ready.load( std::memory_order_acquire ) < writers )
            std::this_thread::yield();

        uint64_t ok = 0;
        uint64_t drop = 0;
        for ( int i = 0; i < lines_per_writer; ++i )
        {
            const std::string line = make_padded_line( id, i );
            if ( sink.push( ( i % 4 ) == 0, line.data(), line.size() ) )
                ++ok;
            else
                ++drop;
            if ( ( i % 120 ) == 0 )
                log.info() << "l|info " << id << ' ' << i << '\n';
            if ( ( i % 240 ) == 0 )
                log.warn() << "l|warn " << id << ' ' << i << '\n';
        }
        const std::string oversized( light_ai::platform::AsyncLogSink::kMaxLineBytes + 96, 'x' );
        static_cast<void>( sink.push( false, oversized.data(), oversized.size() ) );
        accepted_push.fetch_add( ok, std::memory_order_relaxed );
        rejected_push.fetch_add( drop, std::memory_order_relaxed );
    }

    void run_burst_writers( light_ai::platform::AsyncLogSink& sink, const Log& log, int writers, int lines_per_writer,
                            std::atomic<uint64_t>& accepted_push, std::atomic<uint64_t>& rejected_push )
    {
        std::atomic<int> ready { 0 };
        std::vector<std::thread> threads;
        threads.reserve( static_cast<std::size_t>( writers ) );
        for ( int id = 0; id < writers; ++id )
        {
            threads.emplace_back( [&sink, &log, &ready, &accepted_push, &rejected_push, writers, lines_per_writer, id] {
                writer_worker( sink, log, ready, writers, lines_per_writer, id, accepted_push, rejected_push );
            } );
        }
        for ( std::thread& th : threads )
            th.join();
    }

    // What the sink writes for one oversized line (destroying the sink drains it).
    std::string drain_oversized_line( const std::string& line )
    {
        std::ostringstream out_os;
        std::ostringstream err_os;
        {
            light_ai::platform::AsyncLogSink sink( out_os, err_os, false );
            static_cast<void>( sink.push( false, line.data(), line.size() ) );
        }
        return out_os.str();
    }

    // A cut line keeps its newline in the last byte; a cut chunk without one gets none.
    void check_truncation_keeps_newline( TestRun& t )
    {
        constexpr std::size_t k_max = light_ai::platform::AsyncLogSink::kMaxLineBytes;
        const std::string body( k_max + 96, 'x' );
        CHECK_STR_EQ( t, drain_oversized_line( body + '\n' ), std::string( k_max - 1, 'x' ) + '\n' );
        CHECK_STR_EQ( t, drain_oversized_line( body ), std::string( k_max, 'x' ) );
    }
}  // namespace

void test_async_log_sink_threads( TestRun& t )
{
    std::ostringstream out_os;
    std::ostringstream err_os;

    constexpr int k_writers = 8;
    constexpr int k_lines_per_writer = 3000;

    std::atomic<uint64_t> accepted_push { 0 };
    std::atomic<uint64_t> rejected_push { 0 };
    uint64_t dropped_before_dtor = 0;

    {
        light_ai::platform::AsyncLogSink sink( out_os, err_os, false );
        const Log log( sink.sink_fn(), Log::Level::Debug );

        run_burst_writers( sink, log, k_writers, k_lines_per_writer, accepted_push, rejected_push );
        if ( sink.dropped_count() == 0 )
            run_burst_writers( sink, log, k_writers, k_lines_per_writer * 3, accepted_push, rejected_push );

        dropped_before_dtor = sink.dropped_count();
        CHECK( t, dropped_before_dtor >= rejected_push.load( std::memory_order_relaxed ) );
    }  // ~AsyncLogSink(): drains remaining slots and flushes dropped counter

    const std::string out_text = out_os.str();
    const std::string err_text = err_os.str();
    const uint64_t drained_push = static_cast<uint64_t>(
        count_occurrences( out_text, "p|" ) + count_occurrences( err_text, "p|" ) );
    const std::size_t drained_log = count_occurrences( out_text, "l|" ) + count_occurrences( err_text, "l|" );

    CHECK( t, rejected_push.load() > 0 );
    CHECK( t, dropped_before_dtor > 0 );
    CHECK( t, accepted_push.load() > 0 );
    CHECK_EQ( t, drained_push, accepted_push.load() );
    CHECK( t, drained_log > 0 );
    CHECK( t, err_text.find( "log: dropped " ) != std::string::npos );

    check_truncation_keeps_newline( t );
}
