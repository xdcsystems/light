#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "config/cli.hpp"
#include "core/log.hpp"
#include "suites.hpp"

namespace
{
    using Args = std::initializer_list<const char*>;

    // CliOptions over its own Log: both streams are strings the test reads.
    struct Cli
    {
        std::ostringstream out;
        std::ostringstream err;
        Log log { out, err };
        CliOptions opts { log };

        const Config& config() const { return opts.config(); }
    };

    // Runs CliOptions::parse over a copy of the arguments (argv is char**,
    // the copy keeps the literals const).
    CliOptions::ParseStatus parse( Cli& cli, Args args )
    {
        std::vector<std::string> store { "light_ai" };
        store.insert( store.end(), args.begin(), args.end() );
        std::vector<char*> argv;
        for ( std::string& s : store )
            argv.push_back( s.data() );
        return cli.opts.parse( static_cast<int>( argv.size() ), argv.data() );
    }

    bool rejected( Args args )
    {
        Cli cli;
        return parse( cli, args ) == CliOptions::ParseStatus::Error;
    }

    // One parse with its two Log streams: what the user sees on stdout / stderr.
    struct Run
    {
        CliOptions::ParseStatus st;
        std::string out;
        std::string err;
    };

    Run run( Args args )
    {
        Cli cli;
        const CliOptions::ParseStatus st = parse( cli, args );
        return { st, cli.out.str(), cli.err.str() };
    }

    bool error_text( Args args, const std::string& err )
    {
        const Run r = run( args );
        return r.st == CliOptions::ParseStatus::Error && r.out.empty() && r.err == err;
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

namespace
{
    // The exact texts of every rejection, pinned before the stage-6e move to Log (#39).
    void texts( TestRun& t )
    {
        CHECK( t, error_text( { "--no-such-option" }, "unknown option: --no-such-option\n" ) );
        CHECK( t, error_text( { "--frames" }, "missing value for option: --frames\n" ) );
        CHECK( t, error_text( { "--config" }, "missing value for option: --config\n" ) );
        CHECK( t, error_text( { "--frames", "0" }, "invalid value for --frames: '0' (expected >= 1)\n" ) );
        CHECK( t, error_text( { "--cal", "0" }, "invalid value for --cal: '0' (expected 1..10000)\n" ) );
        CHECK( t, error_text( { "--cal-interval", "x" }, "invalid value for --cal-interval: 'x' (expected 1..10000)\n" ) );
        CHECK( t, error_text( { "--conf", "1.5" }, "invalid value for --conf: '1.5' (expected 0.0..1.0)\n" ) );
        CHECK( t, error_text( { "--iou", "-0.1" }, "invalid value for --iou: '-0.1' (expected 0.0..1.0)\n" ) );
        CHECK( t, error_text( { "--pool", "2" }, "invalid value for --pool: '2' (expected 0|1|3)\n" ) );
        CHECK( t, error_text( { "--core", "5" }, "invalid value for --core: '5' (expected 0|1|2|3|4|7)\n" ) );
        CHECK( t, error_text( { "--net-port", "0" }, "invalid value for --net-port: '0' (expected 1..65535)\n" ) );
        CHECK( t, error_text( { "--net-fps", "121" }, "invalid value for --net-fps: '121' (expected 0..120)\n" ) );
        CHECK( t, error_text( { "--net-bitrate", "999" }, "invalid value for --net-bitrate: '999' (expected 1000..100000 kbps)\n" ) );
        CHECK( t, error_text( { "--net-src", "ftp://c/s" },
                              "invalid value for --net-src: 'ftp://c/s' (expected http://host:port/path)\n" ) );
        CHECK( t, error_text( { "--device", "" }, "invalid value for --device: '' (expected /dev/videoN)\n" ) );
        CHECK( t, error_text( { "--cal-dir", "" }, "invalid value for --cal-dir: '' (expected non-empty path)\n" ) );
        CHECK( t, error_text( { "--net-h264" },
                              "--net-h264: preview is off, nothing to stream.\n"
                              "  --net-h264 selects the transport of the --net preview subsystem (MPEG-TS over TCP pull, or UDP\n"
                              "  push with --net-host); add --net to enable the preview itself, e.g.:\n"
                              "    light_ai --net --net-h264\n" ) );
        CHECK( t, error_text( { "--cal-dir", "light_ai_tests_cal" }, "--cal-dir: no --cal series requested, nothing to write.\n"
                                                                    "  --cal-dir sets the directory for the --cal N --cal-interval K\n"
                                                                    "  frame series; the single-shot --dump-cal always writes\n"
                                                                    "  /tmp/cal.ppm and ignores --cal-dir, e.g.:\n"
                                                                    "    light_ai --cal 5 --cal-interval 60 --cal-dir /tmp/calrun\n" ) );
        CHECK( t, error_text( { "--device", "a", "--device", "b", "--device", "c", "--device", "d" },
                              "too many video sources (4): at most 3 sources are supported (one per NPU core)\n" ) );

        // --help: parse() prints nothing; main() prints the usage on stdout through print_usage.
        {
            const Run r = run( { "--help" } );
            CHECK( t, r.st == CliOptions::ParseStatus::Help && r.out.empty() && r.err.empty() );
            const Cli cli;
            std::ostringstream usage;
            cli.opts.print_usage( usage );
            CHECK( t, usage.str().rfind( "usage: light_ai [options]\noptions:\n", 0 ) == 0 );
        }

        // The config file: its own texts, then the line of the first error.
        {
            const TempIni ok( "frames = 5\n" );
            const Run r = run( { "--config", ok.path() } );
            CHECK( t, r.st == CliOptions::ParseStatus::Run && r.err.empty() );
            CHECK( t, r.out == std::string( "config: loaded " ) + ok.path() + "\n" );
        }
        const std::string rejected_line = "config: rejected: light_ai_tests_tmp.ini (line 1)\n";
        {
            const TempIni ini( "no_such_key = 1\n" );
            CHECK( t, error_text( { "--config", ini.path() },
                                  "config: unknown key: 'no_such_key'; valid names: 'light_ai --help'\n" + rejected_line ) );
        }
        {
            const TempIni ini( "[net]\n" );
            CHECK( t, error_text( { "--config", ini.path() },
                                  "config: sections are not supported: [net]; flat key = value only\n" + rejected_line ) );
        }
        {
            const TempIni ini( "net = false\n" );
            CHECK( t, error_text( { "--config", ini.path() }, "config: flag 'net' takes only true/yes/on/1; "
                                                              "a file cannot turn a flag off -- remove the line\n" + rejected_line ) );
        }
        {
            const TempIni ini( "pool = 2\n" );
            CHECK( t, error_text( { "--config", ini.path() }, "invalid value for --pool: '2' (expected 0|1|3)\n"
                                                              "config: rejected value (key 'pool')\n" + rejected_line ) );
        }
        {
            const TempIni ini( "config = other.ini\n" );
            CHECK( t, error_text( { "--config", ini.path() },
                                  "config: nested --config in a config file is not supported\n" + rejected_line ) );
        }
        CHECK( t, error_text( { "--config", "light_ai_tests_missing.ini" }, "config: cannot open file: light_ai_tests_missing.ini\n" ) );
    }
}  // namespace

void test_config( TestRun& t )
{
    texts( t );

    using Status = CliOptions::ParseStatus;

    // Defaults: one V4L2 source on /dev/video20, everything optional is off.
    {
        Cli cli;
        CHECK( t, parse( cli, {} ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.sources.size() == 1 && c.sources[0].kind == SourceKind::V4L2 && c.sources[0].uri == "/dev/video20" );
        CHECK( t, c.frames == 0 && c.infer.pool == 0 && c.infer.core == 0 );
        CHECK( t, !c.stream.enabled && !c.stream.h264 && !c.stream.cpu_draw && !c.cal.dump_one );
        CHECK( t, c.stream.port == 1234 && c.stream.fps == 25 && c.stream.bitrate_kbps == 12000 );
        CHECK_NEAR( t, c.infer.conf_thr, 0.25f, 1e-6f );
        CHECK_NEAR( t, c.infer.iou_thr, 0.45f, 1e-6f );
        CHECK( t, c.cal.frames == 0 && c.cal.interval == 15 && c.cal.dir.empty() && c.box_log.empty() );
    }
    // Values land in their fields.
    {
        Cli cli;
        CHECK( t, parse( cli, { "--frames", "100", "--conf", "0.5", "--iou", "0.3", "--pool", "3", "--core", "7", "--net",
                               "--net-h264", "--net-port", "5000", "--net-fps", "0", "--net-bitrate", "8000", "--cpu-draw",
                               "--model", "m.rknn", "--box-log", "b.csv", "--net-host", "10.0.0.1", "--dump-cal",
                               "--cal", "20", "--cal-interval", "5" } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.frames == 100 && c.infer.pool == 3 && c.infer.core == 7 );
        CHECK( t, c.stream.enabled && c.stream.h264 && c.stream.cpu_draw );
        CHECK( t, c.stream.port == 5000 && c.stream.fps == 0 && c.stream.bitrate_kbps == 8000 );
        CHECK( t, c.infer.model == "m.rknn" && c.box_log == "b.csv" && c.stream.host == "10.0.0.1" );
        CHECK_NEAR( t, c.infer.conf_thr, 0.5f, 1e-6f );
        CHECK_NEAR( t, c.infer.iou_thr, 0.3f, 1e-6f );
        CHECK( t, c.cal.dump_one && c.cal.frames == 20 && c.cal.interval == 5 );
    }
    // Repeatable sources keep the command-line order.
    {
        Cli cli;
        CHECK( t, parse( cli, { "--net-src", "http://cam:8080/stream", "--device", "/dev/video1" } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.sources.size() == 2 );
        CHECK( t, c.sources[0].kind == SourceKind::NetMjpeg && c.sources[0].uri == "http://cam:8080/stream" );
        CHECK( t, c.sources[1].kind == SourceKind::V4L2 && c.sources[1].uri == "/dev/video1" );
    }
    // Integer front-end: leading whitespace and an explicit '+' are accepted.
    {
        Cli plus;
        CHECK( t, parse( plus, { "--frames", "+10" } ) == Status::Run && plus.config().frames == 10 );
        Cli space;
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
        Cli cli;
        CHECK( t, parse( cli, { "--net", "--net-h264" } ) == Status::Run );
        Cli three;
        CHECK( t, parse( three, { "--device", "/dev/video0", "--device", "/dev/video1", "--net-src", "http://c/s" } )
                  == Status::Run );
        CHECK( t, three.config().sources.size() == 3 );
    }
    // --help wins over everything else and never reads a config file.
    {
        Cli cli;
        CHECK( t, parse( cli, { "--no-such-option", "--config", "missing.ini", "--help" } ) == Status::Help );
    }

    // The registry: lookup by full name, usage lists every option once.
    {
        std::ostringstream out;
        std::ostringstream err;
        const Log log( out, err );
        const OptionRegistry reg( log );
        CHECK( t, reg.find( "--pool" ) != nullptr && reg.find( "--pool" )->opt.needs_value );
        CHECK( t, reg.find( "--net" ) != nullptr && !reg.find( "--net" )->opt.needs_value );
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
        Cli cli;
        CHECK( t, parse( cli, { "--config", ini.path() } ) == Status::Run );
        const Config& c = cli.config();
        CHECK( t, c.frames == 50 && c.stream.enabled && c.stream.port == 4321 );
        CHECK_NEAR( t, c.infer.conf_thr, 0.6f, 1e-6f );
        CHECK( t, c.sources.size() == 1 && c.sources[0].uri == "/dev/video5" );

        // Precedence: defaults < config file < CLI; CLI sources replace the file's sources.
        Cli over;
        CHECK( t, parse( over, { "--frames", "7", "--config", ini.path(), "--device", "/dev/video9" } ) == Status::Run );
        CHECK( t, over.config().frames == 7 && over.config().stream.port == 4321 );
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
