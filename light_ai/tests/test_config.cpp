#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "config/cli.hpp"
#include "suites.hpp"

namespace
{
    using Args = std::initializer_list<const char*>;

    // Runs CliOptions::parse over a copy of the arguments (argv is char**,
    // the copy keeps the literals const). Output is muted: rejections are loud by design.
    CliOptions::ParseStatus parse( CliOptions& cli, Args args )
    {
        std::vector<std::string> store { "light_ai" };
        store.insert( store.end(), args.begin(), args.end() );
        std::vector<char*> argv;
        for ( std::string& s : store )
            argv.push_back( s.data() );
        const QuietStreams quiet;
        return cli.parse( static_cast<int>( argv.size() ), argv.data() );
    }

    bool rejected( Args args )
    {
        CliOptions cli;
        return parse( cli, args ) == CliOptions::ParseStatus::Error;
    }

    // Config file in the working directory, removed on scope exit.
    class TempIni
    {
    public:
        explicit TempIni( const char* text )
        {
            std::ofstream f( path() );
            f << text;
        }
        ~TempIni() { std::remove( path() ); }
        TempIni( const TempIni& ) = delete;
        TempIni& operator=( const TempIni& ) = delete;

        const char* path() const { return "light_ai_tests_tmp.ini"; }
    };
}  // namespace

void test_config( TestRun& t )
{
    using Status = CliOptions::ParseStatus;

    // Defaults: one V4L2 source on /dev/video20, everything optional is off.
    {
        CliOptions cli;
        CHECK( t, parse( cli, {} ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.sources.size() == 1 && c.sources[0].kind == SourceKind::V4L2 && c.sources[0].uri == "/dev/video20" );
        CHECK( t, c.device == "/dev/video20" && c.net_src.empty() );
        CHECK( t, c.frames == 0 && c.pool == 0 && c.core == 0 );
        CHECK( t, !c.net && !c.net_h264 && !c.cpu_draw && !c.dump_cal );
        CHECK( t, c.net_port == 1234 && c.net_fps == 25 && c.net_bitrate == 12000 );
        CHECK_NEAR( t, c.conf_thr, 0.25f, 1e-6f );
        CHECK_NEAR( t, c.iou_thr, 0.45f, 1e-6f );
    }
    // Values land in their fields.
    {
        CliOptions cli;
        CHECK( t, parse( cli, { "--frames", "100", "--conf", "0.5", "--iou", "0.3", "--pool", "3", "--core", "7", "--net",
                               "--net-h264", "--net-port", "5000", "--net-fps", "0", "--net-bitrate", "8000", "--cpu-draw",
                               "--model", "m.rknn", "--box-log", "b.csv", "--net-host", "10.0.0.1" } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.frames == 100 && c.pool == 3 && c.core == 7 );
        CHECK( t, c.net && c.net_h264 && c.cpu_draw );
        CHECK( t, c.net_port == 5000 && c.net_fps == 0 && c.net_bitrate == 8000 );
        CHECK( t, c.model == "m.rknn" && c.box_log == "b.csv" && c.net_host == "10.0.0.1" );
        CHECK_NEAR( t, c.conf_thr, 0.5f, 1e-6f );
        CHECK_NEAR( t, c.iou_thr, 0.3f, 1e-6f );
    }
    // Repeatable sources keep the command-line order; the primary fields follow the first.
    {
        CliOptions cli;
        CHECK( t, parse( cli, { "--net-src", "http://cam:8080/stream", "--device", "/dev/video1" } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.sources.size() == 2 );
        CHECK( t, c.sources[0].kind == SourceKind::NetMjpeg && c.sources[0].uri == "http://cam:8080/stream" );
        CHECK( t, c.sources[1].kind == SourceKind::V4L2 && c.sources[1].uri == "/dev/video1" );
        CHECK( t, c.net_src == "http://cam:8080/stream" && c.device.empty() );
    }
    // Integer front-end: leading whitespace and an explicit '+' are accepted.
    {
        CliOptions plus;
        CHECK( t, parse( plus, { "--frames", "+10" } ) == Status::Run && plus.config().frames == 10 );
        CliOptions space;
        CHECK( t, parse( space, { "--frames", " 10" } ) == Status::Run && space.config().frames == 10 );
    }
    // Loud rejections: unknown names, missing values, out-of-range and malformed values.
    CHECK( t, rejected( { "--no-such-option" } ) );
    CHECK( t, rejected( { "--frames" } ) );
    CHECK( t, rejected( { "--config" } ) );
    CHECK( t, rejected( { "--frames", "0" } ) );
    CHECK( t, rejected( { "--frames", "10x" } ) );
    CHECK( t, rejected( { "--frames", "+" } ) );
    CHECK( t, rejected( { "--frames", "99999999999" } ) );
    CHECK( t, rejected( { "--conf", "1.5" } ) );
    CHECK( t, rejected( { "--conf", "abc" } ) );
    CHECK( t, rejected( { "--conf", "nan" } ) );
    CHECK( t, rejected( { "--iou", "-0.1" } ) );
    CHECK( t, rejected( { "--pool", "2" } ) );
    CHECK( t, rejected( { "--core", "5" } ) );
    CHECK( t, rejected( { "--net-port", "70000" } ) );
    CHECK( t, rejected( { "--net-fps", "121" } ) );
    CHECK( t, rejected( { "--net-bitrate", "999" } ) );
    CHECK( t, rejected( { "--net-src", "ftp://cam/stream" } ) );
    CHECK( t, rejected( { "--device", "" } ) );
    // Cross-field checks run after the argv loop.
    CHECK( t, rejected( { "--net-h264" } ) );                   // transport without the preview
    CHECK( t, rejected( { "--cal-dir", "light_ai_tests_cal" } ) );  // directory without the series
    CHECK( t, rejected( { "--device", "/dev/video0", "--device", "/dev/video1", "--device", "/dev/video2", "--device",
                          "/dev/video3" } ) );                  // at most 3 sources
    {
        CliOptions cli;
        CHECK( t, parse( cli, { "--net", "--net-h264" } ) == Status::Run );
        CliOptions three;
        CHECK( t, parse( three, { "--device", "/dev/video0", "--device", "/dev/video1", "--net-src", "http://c/s" } )
                  == Status::Run );
        CHECK( t, three.config().sources.size() == 3 );
    }
    // --help wins over everything else and never reads a config file.
    {
        CliOptions cli;
        CHECK( t, parse( cli, { "--no-such-option", "--config", "missing.ini", "--help" } ) == Status::Help );
    }

    // The registry: lookup by full name, usage lists every option once.
    {
        const OptionRegistry reg;
        CHECK( t, reg.find( "--pool" ) != nullptr && reg.find( "--pool" )->needs_value );
        CHECK( t, reg.find( "--net" ) != nullptr && !reg.find( "--net" )->needs_value );
        CHECK( t, reg.find( "--poo" ) == nullptr && reg.find( "pool" ) == nullptr && reg.find( "" ) == nullptr );
        std::ostringstream usage;
        reg.print_usage( usage );
        const std::string text = usage.str();
        int lines = 0;
        for ( const char ch : text )
            lines += ( ch == '\n' ) ? 1 : 0;
        CHECK( t, lines == 2 + OptionRegistry::kOptionCount );
        CHECK( t, text.find( "  --net-src <value> - " ) != std::string::npos );
        CHECK( t, text.find( "  --cpu-draw - " ) != std::string::npos );
    }

    // Config file: flat key = value through the same registry; '_' and '-' are equivalent.
    {
        const TempIni ini( "frames = 50\nnet = true\nnet_port = 4321\n--conf = 0.6\ndevice = /dev/video5\n" );
        CliOptions cli;
        CHECK( t, parse( cli, { "--config", ini.path() } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.frames == 50 && c.net && c.net_port == 4321 );
        CHECK_NEAR( t, c.conf_thr, 0.6f, 1e-6f );
        CHECK( t, c.sources.size() == 1 && c.sources[0].uri == "/dev/video5" );

        // Precedence: defaults < config file < CLI; CLI sources replace the file's sources.
        CliOptions over;
        CHECK( t, parse( over, { "--frames", "7", "--config", ini.path(), "--device", "/dev/video9" } ) == Status::Run );
        CHECK( t, over.config().frames == 7 && over.config().net_port == 4321 );
        CHECK( t, over.config().sources.size() == 1 && over.config().sources[0].uri == "/dev/video9" );
    }
    {
        const TempIni unknown_key( "no_such_key = 1\n" );
        CHECK( t, rejected( { "--config", unknown_key.path() } ) );
    }
    {
        const TempIni section( "[net]\nport = 1\n" );
        CHECK( t, rejected( { "--config", section.path() } ) );
    }
    {
        const TempIni flag_off( "net = false\n" );  // a file can only turn a flag on
        CHECK( t, rejected( { "--config", flag_off.path() } ) );
    }
    {
        const TempIni bad_value( "pool = 2\n" );
        CHECK( t, rejected( { "--config", bad_value.path() } ) );
    }
    {
        const TempIni nested( "config = other.ini\n" );
        CHECK( t, rejected( { "--config", nested.path() } ) );
    }
    CHECK( t, rejected( { "--config", "light_ai_tests_missing.ini" } ) );
}
