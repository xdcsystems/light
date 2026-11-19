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

    // The "tensor" of frame seed: different bytes per frame, so a file proves
    // which frame it holds.
    std::vector<uint8_t> pixels( int seed = 0 )
    {
        std::vector<uint8_t> px( kBytes );
        for ( size_t i = 0; i < px.size(); ++i )
            px[i] = static_cast<uint8_t>( i * 7 + 1 + static_cast<size_t>( seed ) * 31 );
        return px;
    }

    void check_paths( TestRun& t )
    {
        const CalKind one = CalKind::Single;
        const CalKind ser = CalKind::Series;
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "" ), one, 0 ), "build/cal.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "" ), one, 42 ), "build/cal.ppm" );  // one file, any frame
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 0, 15, "out/x" ), one, 7 ), "out/x/cal.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "" ), ser, 0 ), "dataset/cal_0000.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "" ), ser, 45 ), "dataset/cal_0045.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( false, 5, 15, "out/x" ), ser, 12345 ), "out/x/cal_12345.ppm" );
        // The kind picks the path, not dump_one: with both options each has its own.
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 5, 15, "" ), one, 30 ), "build/cal.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 5, 15, "" ), ser, 30 ), "dataset/cal_0030.ppm" );
        CHECK_STR_EQ( t, cal_dump_path( cal_of( true, 5, 15, "out/x" ), ser, 30 ), "out/x/cal_0030.ppm" );
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

    // --dump-cal --cal 3 --cal-interval 2 over frames 0..9: the single file is
    // frame 0, the series is frames 0, 2, 4; neither overwrites the other.
    void check_both( TestRun& t, const std::string& dir )
    {
        const std::string sub = dir + "/both";
        std::filesystem::create_directories( sub );
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        CalDumper d( log );
        d.init( cal_of( true, 3, 2, sub ), kW, kH );
        for ( int f = 0; f < 10; ++f )
        {
            const std::vector<uint8_t> px = pixels( f );
            d.maybe( f, -1, px.data(), px.size() );
        }
        CHECK_STR_EQ( t, read_file( sub + "/cal.ppm" ), ppm_of( pixels( 0 ) ) );
        CHECK_STR_EQ( t, read_file( sub + "/cal_0000.ppm" ), ppm_of( pixels( 0 ) ) );
        CHECK_STR_EQ( t, read_file( sub + "/cal_0002.ppm" ), ppm_of( pixels( 2 ) ) );
        CHECK_STR_EQ( t, read_file( sub + "/cal_0004.ppm" ), ppm_of( pixels( 4 ) ) );
        CHECK( t, !std::filesystem::exists( sub + "/cal_0006.ppm" ) );  // the cap: 3 files
        int files = 0;
        for ( const auto& e : std::filesystem::directory_iterator( sub ) )
            files += e.is_regular_file() ? 1 : 0;
        CHECK_EQ( t, files, 4 );
        CHECK_STR_EQ( t, out.str(), "calibration dump: " + sub + "/cal.ppm (4x2)\n" );
        CHECK( t, err.str().empty() );
    }

    void check_failures( TestRun& t, const std::string& dir )
    {
        const std::vector<uint8_t> px = pixels();
        // A short tensor with both kinds: nothing usable is written, one error line per kind, no retries.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            CalDumper d( log );
            const std::string sub = dir + "/short";
            std::filesystem::create_directories( sub );
            d.init( cal_of( true, 3, 2, sub ), kW, kH );
            for ( int f = 0; f < 20; ++f )
                d.maybe( f, -1, px.data(), px.size() - 1 );
            CHECK( t, out.str().empty() );
            CHECK_STR_EQ( t, err.str(),
                          "calibration dump failed: " + sub + "/cal.ppm: tensor shorter than the model frame\n"
                          "calibration dump failed: " + sub + "/cal_0000.ppm: tensor shorter than the model frame\n" );
        }
        // A missing directory (the dumper does not create it): the same, once per kind.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            CalDumper d( log );
            const std::string missing = dir + "/no_such_dir";
            d.init( cal_of( true, 3, 2, missing ), kW, kH );
            for ( int f = 0; f < 20; ++f )
                d.maybe( f, -1, px.data(), px.size() );
            CHECK( t, out.str().empty() );
            CHECK_STR_EQ( t, err.str(),
                          "calibration dump failed: " + missing + "/cal.ppm: open failed: No such file or directory\n"
                          "calibration dump failed: " + missing + "/cal_0000.ppm: open failed: No such file or directory\n" );
            CHECK( t, !std::filesystem::exists( missing ) );
        }
        // A broken series next to a working single shot: the series file path is a
        // directory (cannot be opened as a file), the single file is still written.
        {
            std::ostringstream out;
            std::ostringstream err;
            const Log log( out, err );
            CalDumper d( log );
            const std::string sub = dir + "/half";
            std::filesystem::create_directories( sub + "/cal_0000.ppm" );
            d.init( cal_of( true, 3, 2, sub ), kW, kH );
            for ( int f = 0; f < 10; ++f )
                d.maybe( f, -1, px.data(), px.size() );
            CHECK_STR_EQ( t, read_file( sub + "/cal.ppm" ), ppm_of( px ) );
            CHECK_STR_EQ( t, out.str(), "calibration dump: " + sub + "/cal.ppm (4x2)\n" );
            CHECK_STR_EQ( t, err.str(), "calibration dump failed: " + sub + "/cal_0000.ppm: open failed: Is a directory\n" );
            CHECK( t, !std::filesystem::exists( sub + "/cal_0002.ppm" ) );  // the series stopped
        }
    }

    // M-11: a padded tensor (row stride > model_w * 3) dumps the packed
    // picture -- padding dropped, no diagonal shift -- and the size check
    // counts the last row as model_w * 3, not a full stride.
    void check_padded_stride( TestRun& t, const std::string& dir )
    {
        const std::vector<uint8_t> px = pixels();
        constexpr size_t row = static_cast<size_t>( kW ) * 3;
        constexpr size_t stride = row + 6;  // two padding pixels per row
        std::vector<uint8_t> padded( stride * ( kH - 1 ) + row, 0xEE );
        for ( int y = 0; y < kH; ++y )
            std::copy( px.begin() + static_cast<std::ptrdiff_t>( row * y ), px.begin() + static_cast<std::ptrdiff_t>( row * ( y + 1 ) ),
                       padded.begin() + static_cast<std::ptrdiff_t>( stride * y ) );
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        CalDumper d( log );
        const std::string sub = dir + "/padded";
        std::filesystem::create_directories( sub );
        d.init( cal_of( true, 0, 15, sub ), kW, kH, static_cast<uint32_t>( stride ) );
        d.maybe( 0, -1, padded.data(), padded.size() );
        CHECK_STR_EQ( t, read_file( sub + "/cal.ppm" ), ppm_of( px ) );
        CHECK_STR_EQ( t, err.str(), "" );

        // One byte short of the last row: Invalid, nothing written.
        CalDumper s( log );
        const std::string shrt = dir + "/padded_short";
        std::filesystem::create_directories( shrt );
        s.init( cal_of( true, 0, 15, shrt ), kW, kH, static_cast<uint32_t>( stride ) );
        s.maybe( 0, -1, padded.data(), padded.size() - 1 );
        CHECK( t, !std::filesystem::exists( shrt + "/cal.ppm" ) || read_file( shrt + "/cal.ppm" ).empty() );
        CHECK( t, err.str().find( "tensor shorter than the model frame" ) != std::string::npos );
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
    check_both( t, dir );
    check_failures( t, dir );
    check_padded_stride( t, dir );
    std::filesystem::remove_all( dir, ec );
}
