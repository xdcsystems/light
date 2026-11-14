#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/log.hpp"
#include "source/usb_node_locator.hpp"
#include "source_fakes.hpp"
#include "suites.hpp"

namespace
{
    // Trial over a list of nodes that pass; records every node tried.
    struct Trials
    {
        int operator()( const std::string& node, std::string* why )
        {
            tried.push_back( node );
            for ( const std::string& ok : passing )
            {
                if ( ok == node )
                    return 0;
            }
            *why = "S_FMT failed";
            return -1;
        }

    public:
        std::vector<std::string> passing;
        std::vector<std::string> tried;
    };

    int count_of( const std::string& s, const std::string& what )
    {
        int k = 0;
        for ( size_t at = s.find( what ); at != std::string::npos; at = s.find( what, at + what.size() ) )
            ++k;
        return k;
    }
}  // namespace

void test_usb_node_locator( TestRun& t )
{
    // The configured node, present and usable: taken directly, no scan.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeHost host;
        host.present = { "/dev/video20" };
        host.usb = { 20 };
        BasicUsbNodeLocator<FakeHost> loc( host, log );
        Trials tr;
        tr.passing = { "/dev/video20" };
        std::string found;
        CHECK( t, loc.locate( "/dev/video20", {}, tr, found ) == 0 );
        CHECK( t, found == "/dev/video20" );
        CHECK( t, tr.tried.size() == 1 );
        CHECK( t, err.str().empty() );
    }
    // Busy preferred node: never trialed. The scan goes in ascending N, skips
    // busy nodes and adopts the first node whose trial passes.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeHost host;
        host.present = { "/dev/video20", "/dev/video21", "/dev/video2" };
        host.usb = { 21, 20, 2 };
        BasicUsbNodeLocator<FakeHost> loc( host, log );
        Trials tr;
        tr.passing = { "/dev/video20", "/dev/video21" };
        std::string found;
        CHECK( t, loc.locate( "/dev/video20", { "/dev/video20" }, tr, found ) == 0 );
        CHECK( t, found == "/dev/video21" );
        CHECK( t, ( tr.tried == std::vector<std::string> { "/dev/video2", "/dev/video21" } ) );
        CHECK( t, err.str().find( "camera: trying /dev/video20" ) == std::string::npos );
        CHECK( t, err.str().find( "camera: node /dev/video20 not present, scanning /dev/video*\n" ) != std::string::npos );
    }
    // Preferred present but failing: logged, then excluded from the scan.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeHost host;
        host.present = { "/dev/video20", "/dev/video22" };
        host.usb = { 20, 22 };
        BasicUsbNodeLocator<FakeHost> loc( host, log );
        Trials tr;
        tr.passing = { "/dev/video22" };
        std::string found;
        CHECK( t, loc.locate( "/dev/video20", {}, tr, found ) == 0 );
        CHECK( t, found == "/dev/video22" );
        CHECK( t, ( tr.tried == std::vector<std::string> { "/dev/video20", "/dev/video22" } ) );
        CHECK( t, err.str().find( "camera: reconnect setup on /dev/video20 failed: S_FMT failed\n" ) != std::string::npos );
    }
    // Nothing usable: -1, out untouched. The scan line is throttled to one per 30 s.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        FakeHost host;
        BasicUsbNodeLocator<FakeHost> loc( host, log );
        Trials tr;
        std::string found = "unchanged";
        CHECK( t, loc.locate( "/dev/video20", {}, tr, found ) == -1 );
        CHECK( t, found == "unchanged" );
        host.now += 2000;
        CHECK( t, loc.locate( "/dev/video20", {}, tr, found ) == -1 );
        CHECK( t, count_of( err.str(), "scanning /dev/video*" ) == 1 );
        host.now += kScanLogThrottleMs;
        CHECK( t, loc.locate( "/dev/video20", {}, tr, found ) == -1 );
        CHECK( t, count_of( err.str(), "scanning /dev/video*" ) == 2 );
        CHECK( t, tr.tried.empty() );
    }
}
