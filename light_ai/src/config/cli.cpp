// The parameter layer. The option registry
// (names, ranges, error texts) lives in config/option_registry.{h,cpp};
// the INI config-file layer lives in config/config_file.{h,cpp}
// (externals/inih r62, BSD-3-Clause); this file owns the CliOptions
// orchestration (pre-pass, argv loop, cross-field checks).
// Precedence: defaults < config file < CLI.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "config/cli.hpp"
#include "config/config_file.h"

namespace
{
    bool ensure_writable_dir( const std::string& dir )
    {
        std::error_code ec;
        std::filesystem::create_directories( dir, ec );
        if ( ec )
        {
            std::cerr << "invalid value for --cal-dir: '" << dir << "' (" << ec.message() << ")\n";
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
            std::cerr << "invalid value for --cal-dir: '" << dir << "' (not writable)\n";
            return false;
        }
        return true;
    }

    void finalize_sources( std::vector<SourceEndpoint>& ini_srcs, Config& cfg )
    {
        if ( cfg.sources.empty() )
            cfg.sources = std::move( ini_srcs );
        if ( cfg.sources.empty() )
            cfg.sources.push_back( { SourceKind::V4L2, "/dev/video20" } );
    }
}  // namespace

// ---- CliOptions implementation ----

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
        std::cerr << "--net-h264: preview is off, nothing to stream.\n"
                     "  --net-h264 selects the transport of the --net "
                     "preview subsystem (MPEG-TS over TCP pull, or UDP\n"
                     "  push with --net-host); add --net to enable the "
                     "preview itself, e.g.:\n"
                     "    light_ai --net --net-h264\n";
        return 2;
    }

    // --cal-dir only makes sense with the --cal series; --dump-cal always
    // writes /tmp/cal.ppm and is unaffected. Post-parse for the same
    // reason as --net-h264: order of argv is arbitrary.
    if ( !cfg_.cal.dir.empty() && cfg_.cal.frames == 0 )
    {
        std::cerr << "--cal-dir: no --cal series requested, nothing to write.\n"
                     "  --cal-dir sets the directory for the --cal N --cal-interval K\n"
                     "  frame series; the single-shot --dump-cal always writes\n"
                     "  /tmp/cal.ppm and ignores --cal-dir, e.g.:\n"
                     "    light_ai --cal 5 --cal-interval 60 --cal-dir /tmp/calrun\n";
        return 2;
    }
    if ( !cfg_.cal.dir.empty() && !ensure_writable_dir( cfg_.cal.dir ) )
        return 2;

    if ( cfg_.sources.size() > 3 )
    {
        std::cerr << "too many video sources (" << cfg_.sources.size()
                  << "): at most 3 sources are supported (one per NPU core)\n";
        return 2;
    }

    return 0;
}

CliOptions::ParseStatus CliOptions::parse( int argc, char** argv )
{
    // Pre-pass: locate --config and --help before anything else. The
    // file layer applies BEFORE the CLI loop, so later CLI flags
    // override it (defaults < config file < CLI). --help skips the
    // load: help must never depend on a file.
    std::string config_path;
    bool want_help = false;
    for ( int i = 1; i < argc; ++i )
    {
        if ( !std::strcmp( argv[i], "--config" ) )
        {
            if ( i + 1 >= argc )
            {
                std::cerr << "missing value for option: --config\n";
                return ParseStatus::Error;
            }
            config_path = argv[++i];
        }
        else if ( !std::strcmp( argv[i], "-h" ) || !std::strcmp( argv[i], "--help" ) )
            want_help = true;
    }
    if ( want_help )
    {
        reg_.print_usage( std::cout );
        return ParseStatus::Help;
    }
    if ( !config_path.empty() )
    {
        ConfigFileLoader loader( reg_ );
        if ( !loader.load( config_path.c_str(), cfg_ ) )
            return ParseStatus::Error;
    }
    std::vector<SourceEndpoint> ini_srcs = std::move( cfg_.sources );
    cfg_.sources.clear();

    for ( int i = 1; i < argc; ++i )
    {
        const char* opt = argv[i];
        const OptionRegistry::Option* o = reg_.find( opt );
        if ( o == nullptr )
        {
            std::cerr << "unknown option: " << opt << '\n';
            return ParseStatus::Error;
        }
        const char* val = nullptr;
        if ( o->needs_value )
        {
            if ( i + 1 >= argc )
            {
                std::cerr << "missing value for option: " << opt << '\n';
                return ParseStatus::Error;
            }
            val = argv[++i];
        }
        if ( !o->apply( cfg_, val ) )
            return ParseStatus::Error;
    }

    finalize_sources( ini_srcs, cfg_ );
    return validate() == 0 ? ParseStatus::Run : ParseStatus::Error;
}
