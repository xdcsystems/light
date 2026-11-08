// cli.cpp (2026-11-08): the parameter layer. The option registry
// (names, ranges, error texts) lives in option_registry.{hpp,cpp};
// this file owns the INI config-file layer (externals/inih r62,
// BSD-3-Clause) and the CliOptions orchestration (pre-pass, argv
// loop, cross-field checks).
// Precedence: defaults < config file < CLI.

#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

#include "cli.hpp"
#include "ini.h"

namespace
{
    // Config-file layer (2026-11-08): inih (externals/inih, BSD-3-Clause)
    // parses a flat `key = value` file THROUGH the same option registry
    // the CLI uses -- one registry, one validation path, identical error
    // text in both sources. Flat namespace only: a section header is a
    // loud error (the handler is invoked at every section start via
    // INI_CALL_HANDLER_ON_NEW_SECTION=1, set in CMakeLists.txt).
    // HANDLER CONVENTION: 0 = error, nonzero = success (per ini.h:
    // "nonzero on success, zero on error") -- the returns below follow
    // it; the 2026-11-08 bug had them inverted. Values are trimmed by
    // inih; max line length 200 chars (inih default), which bounds
    // string values (paths, URLs).
    struct CfgFileCtx
    {
        Config* cfg;
        const OptionRegistry* reg;
    };

    // Standard inih handler signature (no line-number parameter): ini_parse() returns
    // the FIRST-error line (it keeps parsing past errors), reported
    // once by load_config_file.
    int cfg_file_handler( void* user, const char* section, const char* key,
            const char* value )
    {
        CfgFileCtx* cx = static_cast<CfgFileCtx*>( user );
        // inih passes section = "" (NOT nullptr) for entries parsed
        // before any [section] header -- the empty name is the normal
        // flat case, only a real named section is an error.
        if ( section && *section != '\0' )
        {
            std::cerr << "config: sections are not supported: [" << section
                      << "]; flat key = value only\n";
            return 0;
        }
        // A NULL name is the section-start call (CALL_HANDLER=1); any
        // section is rejected above, so this is an error in every case
        // that can reach it (0 = error in the inih convention).
        if ( !key )
            return 0;
        // Normalization: leading dashes stripped, '_' -> '-', then the
        // "--" prefix re-attached -- both `net_h264 = true` and
        // `--net-h264 = true` land on the same table row.
        std::string norm;
        for ( const char* p = key; *p; ++p )
        {
            if ( *p == '-' && norm.empty() )
                continue;
            norm += ( *p == '_' ) ? '-' : *p;
        }
        if ( norm.empty() )
            return 0;
        if ( norm == "config" )
        {
            std::cerr << "config: nested --config in a config file is not "
                      << "supported\n";
            return 0;
        }
        const OptionRegistry::Option* o =
                cx->reg->find( ( "--" + norm ).c_str() );
        if ( o == nullptr )
        {
            std::cerr << "config: unknown key: '" << key
                      << "'; valid names: 'light_ai --help'\n";
            return 0;
        }
        if ( !o->needs_value )
        {
            // Flags default to off and are only ever set: the file may
            // turn them on (true/yes/on/1, case-insensitive), never off.
            std::string v = value ? value : "";
            for ( char& ch : v )
                ch = static_cast<char>( std::tolower( static_cast<unsigned char>( ch ) ) );
            if ( v != "1" && v != "true" && v != "yes" && v != "on" )
            {
                std::cerr << "config: flag '" << key << "' takes only true/yes/on/1; "
                          << "a file cannot turn a flag off -- remove the line\n";
                return 0;
            }
            return o->apply( *cx->cfg, nullptr ) ? 1 : 0;
        }
        if ( !o->apply( *cx->cfg, value ? value : "" ) )
        {
            // The lambda already printed its value_error; the error line
            // comes from the ini_parse() return (the first error line,
            // parsing continues) and is reported by load_config_file.
            std::cerr << "config: rejected value (key '" << key << "')\n";
            return 0;
        }
        return 1;
    }
// The INI config-file layer: inih parses the file through the same
// option registry the CLI uses (one validation path for both sources).
// A free function, not a method (2026-11-08): it mutates the config
// only through a void* user-context, which clang-tidy cannot see, so
// as a method it is a standing candidate for
// readability-make-member-function-const; as a free function taking
// Config& the mutation is explicit.
bool load_config_file( const char* path, Config& cfg, const OptionRegistry& reg )
{
    CfgFileCtx cx { &cfg, &reg };
    const int rc = ini_parse( path, cfg_file_handler, &cx );
    if ( rc != 0 )
    {
        // ini.h: -1 = file open error, -2 = memory allocation error
        // (only with INI_USE_STACK=0 -- unreachable in this build, kept
        // for completeness); any other nonzero = the first error line.
        if ( rc == -1 )
            std::cerr << "config: cannot open file: " << path << '\n';
        else if ( rc == -2 )
            std::cerr << "config: out of memory while parsing: " << path << '\n';
        else
            std::cerr << "config: rejected: " << path << " (line " << rc << ")\n";
        return false;
    }
    std::cout << "config: loaded " << path << '\n';
    return true;
}

}  // namespace

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
    if ( !config_path.empty() && !load_config_file( config_path.c_str(), cfg_, reg_ ) )
        return ParseStatus::Error;

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
