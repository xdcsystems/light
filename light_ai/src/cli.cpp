// cli.cpp (2026-11-08): the parameter layer. The option registry
// (names, ranges, error texts) lives in option_registry.{hpp,cpp};
// the INI config-file layer lives in config_file.{hpp,cpp}
// (externals/inih r62, BSD-3-Clause); this file owns the CliOptions
// orchestration (pre-pass, argv loop, cross-field checks).
// Precedence: defaults < config file < CLI.

#include <cstring>
#include <iostream>
#include <string>

#include "cli.hpp"
#include "config_file.hpp"

// ---- CliOptions implementation ----

// Cross-field checks: post-parse, because argv order is arbitrary.
// Every block moved verbatim from main.cpp (2026-11-08).
int CliOptions::validate() const
{
    // --net-h264 is a variant of the preview subsystem, not an independent
    // switch: the whole preview setup (encoder, gpu drawer, diagnostics)
    // lives under cfg_.net, so --net-h264 without --net silently runs a
    // stream-less pipeline. Fail loudly instead (checked here,
    // post-parse, because --net may come after --net-h264 on the
    // command line).
    if ( cfg_.net_h264 && !cfg_.net )
    {
        std::cerr << "--net-h264: preview is off, nothing to stream.\n"
                     "  --net-h264 selects the transport of the --net "
                     "preview subsystem (MPEG-TS over TCP pull, or UDP\n"
                     "  push with --net-host); add --net to enable the "
                     "preview itself, e.g.:\n"
                     "    light_ai --net --net-h264\n";
        return 2;
    }

    // A network source replaces the USB camera entirely; the frame
    // geometry, however, still comes from --w/--h: the RGA letterbox
    // mapping and the encoder band are computed from them BEFORE the
    // first frame arrives, and a mismatch with the phone's actual
    // stream would silently stretch the image (the "substituted
    // source is worse than a loud one" principle, M3.8). The user
    // must state the phone's resolution explicitly.
    // Mutual exclusion, loud version: --net-src clears c.device when it
    // comes AFTER --device on the command line; the reverse order leaves
    // both non-empty and one of them is silently ignored (the winner
    // depends on argv order). Refuse instead of guessing.
    if ( !cfg_.net_src.empty() && !cfg_.device.empty() )
    {
        std::cerr << "--device and --net-src are mutually exclusive; both are set:\n"
                     "  --device: " << cfg_.device << "\n"
                     "  --net-src: " << cfg_.net_src << "\n"
                     "  Pass exactly one of them; --net-src replaces the USB camera entirely.\n";
        return 2;
    }
    if ( !cfg_.net_src.empty() )
    {
        std::cerr << "net-src: " << cfg_.net_src << "\n"
                     "  NOTE: --w/--h must match the phone's stream resolution\n"
                     "  (IP Webcam: the same values set in the app), e.g.:\n"
                     "    light_ai --net-src http://<phone-ip>:8080/video --w 1280 --h 720\n";
    }

    // --cal-dir only makes sense with the --cal series; --dump-cal always
    // writes /tmp/cal.ppm and is unaffected. Post-parse for the same
    // reason as --net-h264: order of argv is arbitrary.
    if ( !cfg_.cal_dir.empty() && cfg_.cal_frames == 0 )
    {
        std::cerr << "--cal-dir: no --cal series requested, nothing to write.\n"
                     "  --cal-dir sets the directory for the --cal N --cal-interval K\n"
                     "  frame series; the single-shot --dump-cal always writes\n"
                     "  /tmp/cal.ppm and ignores --cal-dir, e.g.:\n"
                     "    light_ai --cal 5 --cal-interval 60 --cal-dir /tmp/calrun\n";
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

    return validate() == 0 ? ParseStatus::Run : ParseStatus::Error;
}
