// The parameter layer. The option registry
// (names, ranges, error texts) lives in config/option_registry.{h,cpp};
// the INI config-file layer lives in config/config_file.{h,cpp}
// (externals/inih r62, BSD-3-Clause); this file owns the CliOptions
// orchestration (pre-pass, argv loop, cross-field checks).
// Precedence: defaults < config file < CLI.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "config/cli.hpp"
#include "config/config_file.h"

namespace
{
    bool ensure_writable_dir( const std::string& dir, const Log& log )
    {
        std::error_code ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
        {
            log.error() << "invalid value for --cal-dir: '" << dir << "' (" << ec.message() << ")\n";
            return false;
        }
        const std::string probe = dir + "/.light_ai_write_test";
        bool ok = false;
        {
            std::ofstream f( probe );
            ok = f.good();
        }
        std::filesystem::remove( probe, ec );
        if ( !ok )
        {
            log.error() << "invalid value for --cal-dir: '" << dir << "' (not writable)\n";
            return false;
        }
        return true;
    }

}  // namespace

// ---- CliOptions implementation ----

CliOptions::CliOptions( const Log& log ) : reg_( log ), log_( log ) {}

// Cross-field checks: post-parse, because argv order is arbitrary.
int CliOptions::validate() const
{
    // --net-h264 is a variant of the preview subsystem, not an independent
    // switch: the whole preview setup (encoder, gpu drawer, diagnostics)
    // lives under cfg_.stream.enabled, so --net-h264 without --net silently runs a
    // stream-less pipeline. Fail loudly instead (checked here,
    // post-parse, because --net may come after --net-h264 on the
    // command line).
    if ( cfg_.stream.h264 && !cfg_.stream.enabled )
    {
        log_.error() << "--net-h264: preview is off, nothing to stream.\n"
                        "  --net-h264 selects the transport of the --net "
                        "preview subsystem (MPEG-TS over TCP pull, or UDP\n"
                        "  push with --net-host); add --net to enable the "
                        "preview itself, e.g.:\n"
                        "    light_ai --net --net-h264\n";
        return 2;
    }

    // --cal-dir only makes sense with something to write into it: the --cal
    // series or the single-shot --dump-cal (<cal-dir>/cal.ppm). Post-parse for
    // the same reason as --net-h264: order of argv is arbitrary.
    if ( !cfg_.cal.dir.empty() && cfg_.cal.frames == 0 && !cfg_.cal.dump_one )
    {
        log_.error() << "--cal-dir: no --cal series or --dump-cal requested, nothing to write.\n"
                        "  --cal-dir sets the directory for the --cal N --cal-interval K\n"
                        "  frame series (default dataset/) and for the single-shot\n"
                        "  --dump-cal (default build/cal.ppm), e.g.:\n"
                        "    light_ai --cal 5 --cal-interval 60 --cal-dir build/calrun\n";
        return 2;
    }
    if ( !cfg_.cal.dir.empty() && !ensure_writable_dir( cfg_.cal.dir, log_ ) )
        return 2;

    if ( cfg_.sources.size() > 4 )
    {
        log_.error() << "too many video sources (" << cfg_.sources.size()
                     << "): at most 4 sources are supported (2x2 grid max)\n";
        return 2;
    }

    return 0;
}

// Pre-pass: locate --config and --help before anything else. The file
// layer applies BEFORE the CLI loop, so later CLI flags override it
// (defaults < config file < CLI). --help skips the load: help must never
// depend on a file. A --config without its value is an error even after
// --help (the scan stops there, as before).
CliOptions::ParseStatus CliOptions::prepass( int argc, char** argv, std::string& config_path ) const
{
    bool want_help = false;
    for ( int i = 1; i < argc; ++i )
    {
        if ( !std::strcmp( argv[i], "--config" ) )
        {
            if ( i + 1 >= argc )
            {
                log_.error() << "missing value for option: --config\n";
                return ParseStatus::Error;
            }
            config_path = argv[++i];
        }
        else if ( !std::strcmp( argv[i], "-h" ) || !std::strcmp( argv[i], "--help" ) )
            want_help = true;
    }
    return want_help ? ParseStatus::Help : ParseStatus::Run;
}

bool CliOptions::apply_argv( int argc, char** argv )
{
    bool saw_cli_src = false;
    for ( int i = 1; i < argc; ++i )
    {
        const char* opt = argv[i];
        const OptionRegistry::Entry* e = reg_.find( opt );
        if ( e == nullptr )
        {
            log_.error() << "unknown option: " << opt << '\n';
            return false;
        }
        const char* val = nullptr;
        if ( e->opt.needs_value )
        {
            if ( i + 1 >= argc )
            {
                log_.error() << "missing value for option: " << opt << '\n';
                return false;
            }
            val = argv[++i];
        }
        if ( !saw_cli_src && ( std::strcmp( opt, "--device" ) == 0 || std::strcmp( opt, "--net-src" ) == 0 ) )
        {
            cfg_.sources.clear();
            saw_cli_src = true;
        }
        if ( !reg_.apply( *e, cfg_, val ) )
            return false;
    }
    return true;
}

CliOptions::ParseStatus CliOptions::parse( int argc, char** argv )
{
    std::string config_path;
    const ParseStatus pre = prepass( argc, argv, config_path );
    if ( pre != ParseStatus::Run )
        return pre;
    if ( !config_path.empty() )
    {
        const ConfigFileLoader loader( reg_, log_ );
        if ( !loader.load( config_path.c_str(), cfg_ ) )
            return ParseStatus::Error;
    }
    if ( !apply_argv( argc, argv ) )
        return ParseStatus::Error;
    if ( cfg_.sources.empty() )
        cfg_.sources.push_back( { SourceKind::V4L2, "/dev/video20" } );
    return validate() == 0 ? ParseStatus::Run : ParseStatus::Error;
}
