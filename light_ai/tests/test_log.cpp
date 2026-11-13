#include <ios>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "suites.hpp"

namespace
{
    // An operand that logs while its caller's Line is still open.
    int logs_then_returns( const Log& log, int v )
    {
        log.info() << "nested\n";
        return v;
    }
}  // namespace

void test_log( TestRun& t )
{
    // Routing: info/debug -> out, warn/error -> err; default threshold drops debug.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        log.debug() << "d\n";
        log.info() << "source: geometry " << 1280 << 'x' << 960 << '\n';
        log.warn() << "probe s" << 1 << ": start failed\n";
        log.error() << "Fatal: " << std::string( "boom" ) << '\n';
        CHECK( t, out.str() == "source: geometry 1280x960\n" );
        CHECK( t, err.str() == "probe s1: start failed\nFatal: boom\n" );
        CHECK( t, !log.enabled( Log::Level::Debug ) && log.enabled( Log::Level::Info ) && log.enabled( Log::Level::Error ) );
    }
    // Threshold: Debug shows everything, Warn hides info, Off hides all.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log verbose( out, err, Log::Level::Debug );
        verbose.debug() << "d\n";
        verbose.info() << "i\n";
        CHECK( t, out.str() == "d\ni\n" );

        std::ostringstream out2;
        std::ostringstream err2;
        const Log quiet( out2, err2, Log::Level::Warn );
        quiet.info() << "i\n";
        quiet.warn() << "w\n";
        quiet.error() << "e\n";
        CHECK( t, out2.str().empty() && err2.str() == "w\ne\n" );

        std::ostringstream out3;
        std::ostringstream err3;
        const Log off( out3, err3, Log::Level::Off );
        off.info() << "i\n";
        off.error() << "e\n";
        CHECK( t, out3.str().empty() && err3.str().empty() );
    }
    // Manipulators pass through; a dropped line still evaluates its operands
    // (side effects must not depend on the log level).
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        log.info() << "npu: core mask 0x" << std::hex << 7 << std::dec << ' ' << 10 << std::endl;
        CHECK( t, out.str() == "npu: core mask 0x7 10\n" );

        int calls = 0;
        const Log off( out, err, Log::Level::Off );
        off.info() << ++calls << '\n';
        CHECK( t, calls == 1 );
    }
    // An operand that logs itself nests (recursive mutex) instead of deadlocking.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        log.info() << "outer " << logs_then_returns( log, 5 ) << '\n';
        CHECK( t, out.str().find( "nested\n" ) != std::string::npos );
        CHECK( t, out.str().find( "5\n" ) != std::string::npos );
    }
    // One statement is one uninterrupted line across threads.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        constexpr int k_threads = 4;
        constexpr int k_lines = 500;
        std::vector<std::thread> pool;
        for ( int id = 0; id < k_threads; ++id )
            pool.emplace_back( [&log, id] {
                for ( int n = 0; n < k_lines; ++n )
                    log.info() << "worker " << id << " line " << n << " tail" << '\n';
            } );
        for ( std::thread& th : pool )
            th.join();

        std::istringstream in( out.str() );
        std::string line;
        int lines = 0;
        bool intact = true;
        while ( std::getline( in, line ) )
        {
            ++lines;
            std::istringstream ls( line );
            std::string w1;
            std::string w2;
            std::string w3;
            int id = -1;
            int n = -1;
            ls >> w1 >> id >> w2 >> n >> w3;
            intact = intact && w1 == "worker" && w2 == "line" && w3 == "tail" && id >= 0 && id < k_threads && n >= 0 && n < k_lines
                     && ls.eof();
        }
        CHECK( t, lines == k_threads * k_lines );
        CHECK( t, intact );
    }
}
