// CalDumper: the dump path (cal_dump_path) and the file sink end to end. The
// tensor fd is -1, so DmaSync is a no-op and the "tensor" is a plain buffer.
// Files go under the test cwd (build/tests, scripts/test.sh), into a directory
// the suite creates and removes.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "cal_dumper.hpp"
#include "check.hpp"
#include "config/config.hpp"
#include "core/log.hpp"
#include "suites.hpp"

namespace
{
    constexpr int kW = 4;
    constexpr int kH = 2;
    constexpr size_t kBytes = static_cast<size_t>( kW ) * kH * 3;

    CalConfig cal_of( bool dump_one, int frames, int interval, const std::string& dir )
    {
        CalConfig c;
        c.dump_one = dump_one;
        c.frames = frames;
        c.interval = interval;
        c.dir = dir;
        return c;
    }

    std::string read_file( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        return std::string( std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() );
    }

    // The P6 file CalDumper writes for a kW x kH model over px.
    std::string ppm_of( const std::vector<uint8_t>& px )
    {
        return "P6\n" + std::to_string( kW ) + ' ' + std::to_string( kH ) + "\n255\n" + std::string( px.begin(), px.end() );
    }

    std::vector<uint8_t> pixels()
    {
        std::vector<uint8_t> px( kBytes );
        for ( size_t i = 0; i < px.size(); ++i )
            px[i] = static_cast<uint8_t>( i * 7 + 1 );
        return px;
    }

    void check_paths( TestRun& t )
    {
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "" ), 0 ), "build/cal.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "" ), 42 ), "build/cal.ppm" );  // one file, any frame
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "out/x" ), 7 ), "out/x/cal.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "" ), 0 ), "dataset/cal_0000.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "" ), 45 ), "dataset/cal_0045.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "out/x" ), 12345 ), "out/x/cal_12345.ppm" );
        // --dump-cal with --cal: the single-shot path wins (the precedence of today).
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 5, 15, "" ), 30 ), "build/cal.ppm" );
    }

    void check_single_shot( TestRun& t, const std::string& dir )
    {
        const std::vector<uint8_t> px = pixels();
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        CalDumper d( log );
        d.maybe( 0, -1, px.data(), px.size() );  // before init(): a no-op
        d.init( cal_of( true, 0, 15, dir ), kW, kH );
        for ( int f = 0; f < 40; ++f )
            d.maybe( f, -1, px.data(), px.size() );  // ONE dump per run, not one per interval
        const std::string path = dir + "/cal.ppm";
        CHECK_STR_EQ( t, read_file( path ), ppm_of( px ) );
        CHECK_STR_EQ( t, out.str(), "calibration dump: " + path + " (4x2)\n" );
        CHECK( t, err.str().empty() );
    }

    void check_series( TestRun& t, const std::string& dir )
    {
        const std::vector<uint8_t> px = pixels();
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        CalDumper d( log );
        d.init( cal_of( false, 2, 3, dir ), kW, kH );
        for ( int f = 0; f < 10; ++f )
            d.maybe( f, -1, px.data(), px.size() );
        // Frames 0 and 3: the stride is 3, the cap is 2 files.
        CHECK_STR_EQ( t, read_file( dir + "/cal_0000.ppm" ), ppm_of( px ) );
        CHECK_STR_EQ( t, read_file( dir + "/cal_0003.ppm" ), ppm_of( px ) );
        CHECK( t, !std::filesystem::exists( dir + "/cal_0006.ppm" ) );
        CHECK( t, out.str().empty() && err.str().empty() );  // the series is silent
    }

    void check_failures( TestRun& t, const std::string& dir )
    {
        const std::vector<uint8_t> px = pixels();
        // A short tensor: nothing is written, one error line with the path.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            CalDumper d( log );
            const std::string sub = dir + "/short";
            std::filesystem::create_directories( sub );
            d.init( cal_of( true, 0, 15, sub ), kW, kH );
            for ( int f = 0; f < 20; ++f )
                d.maybe( f, -1, px.data(), px.size() - 1 );
            CHECK( t, out.str().empty() );
            CHECK_STR_EQ( t, err.str(), "calibration dump failed: " + sub + "/cal.ppm\n" );
        }
        // A missing directory (the dumper does not create it): the same line, once.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            CalDumper d( log );
            const std::string missing = dir + "/no_such_dir";
            d.init( cal_of( true, 0, 15, missing ), kW, kH );
            for ( int f = 0; f < 20; ++f )
                d.maybe( f, -1, px.data(), px.size() );
            CHECK( t, out.str().empty() );
            CHECK_STR_EQ( t, err.str(), "calibration dump failed: " + missing + "/cal.ppm\n" );
            CHECK( t, !std::filesystem::exists( missing ) );
        }
    }
}  // namespace

void test_cal_dumper( TestRun& t )
{
    check_paths( t );

    const std::string dir = "light_ai_tests_cal_dumper";
    std::error_code ec;
    std::filesystem::remove_all( dir, ec );
    std::filesystem::create_directories( dir, ec );
    CHECK( t, !ec );
    check_single_shot( t, dir );
    check_series( t, dir );
    check_failures( t, dir );
    std::filesystem::remove_all( dir, ec );
}
