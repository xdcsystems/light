// The INI config-file layer. inih parses
// a flat `key = value` file THROUGH the option registry the CLI uses
// -- one registry, one validation path, identical error text in both
// sources. Flat namespace only: a section header is a loud error (the
// handler is invoked at every section start via
// INI_CALL_HANDLER_ON_NEW_SECTION=1, set in CMakeLists.txt).

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <ini.h>

#include "config/cli.hpp"
#include "config/config_file.h"
#include "config/option_registry.h"
#include "core/log.hpp"

namespace
{
    // HANDLER CONVENTION: 0 = error, nonzero = success (per ini.h:
    // "nonzero on success, zero on error") -- the returns below follow it; 
    struct CfgFileCtx
    {
        Config* cfg;
        const OptionRegistry* reg;
        const Log* log;
    };

    // Normalization: leading dashes stripped, '_' -> '-', then the
    // "--" prefix re-attached -- both `net_h264 = true` and
    // `--net-h264 = true` land on the same table row.
    std::string normalize_key( std::string_view key )
    {
        std::string norm;
        for ( const char ch : key )
        {
            if ( ch == '-' && norm.empty() )
                continue;
            norm += ( ch == '_' ) ? '-' : ch;
        }
        return norm;
    }

    // Flags default to off and are only ever set: the file may
    // turn them on (true/yes/on/1, case-insensitive), never off.
    bool flag_truthy( const char* value )
    {
        std::string v = value ? value : "";
        std::transform( v.begin(), v.end(), v.begin(), []( unsigned char ch ) {
            return static_cast<char>( std::tolower( ch ) );
        } );
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    // Standard inih handler signature (no line-number parameter):
    // ini_parse() returns the FIRST-error line (it keeps parsing past
    // errors), reported once by ConfigFileLoader::load.
    int cfg_file_handler( void* user, const char* section, const char* key,
            const char* value )
    {
        CfgFileCtx* cx = static_cast<CfgFileCtx*>( user );
        // inih passes section = "" (NOT nullptr) for entries parsed
        // before any [section] header -- the empty name is the normal
        // flat case, only a real named section is an error.
        if ( section && *section != '\0' )
        {
            cx->log->error() << "config: sections are not supported: [" << section << "]; flat key = value only\n";
            return 0;
        }
        // A NULL name is the section-start call (CALL_HANDLER=1); any
        // section is rejected above, so this is an error in every case
        // that can reach it (0 = error in the inih convention).
        if ( !key )
            return 0;
        const std::string norm = normalize_key( key );
        if ( norm.empty() )
            return 0;
        if ( norm == "config" )
        {
            cx->log->error() << "config: nested --config in a config file is not supported\n";
            return 0;
        }
        const OptionRegistry::Entry* e = cx->reg->find( "--" + norm );
        if ( e == nullptr )
        {
            cx->log->error() << "config: unknown key: '" << key << "'; valid names: 'light_ai --help'\n";
            return 0;
        }
        if ( !e->opt.needs_value )
        {
            if ( !flag_truthy( value ) )
            {
                cx->log->error() << "config: flag '" << key << "' takes only true/yes/on/1; "
                                 << "a file cannot turn a flag off -- remove the line\n";
                return 0;
            }
            return cx->reg->apply( *e, *cx->cfg, nullptr ) ? 1 : 0;
        }
        if ( !cx->reg->apply( *e, *cx->cfg, value ? value : "" ) )
        {
            // The registry already logged its "invalid value" line; the error
            // line comes from the ini_parse() return (the first error line,
            // parsing continues) and is reported by load().
            cx->log->error() << "config: rejected value (key '" << key << "')\n";
            return 0;
        }
        return 1;
    }
}  // namespace

ConfigFileLoader::ConfigFileLoader( const OptionRegistry& reg, const Log& log )
    : reg_( &reg )
    , log_( &log )
{
}

bool ConfigFileLoader::load( const char* path, Config& cfg ) const
{
    CfgFileCtx cx { &cfg, reg_, log_ };
    const int rc = ini_parse( path, cfg_file_handler, &cx );
    if ( rc != 0 )
    {
        // ini.h: -1 = file open error, -2 = memory allocation error
        // (only with INI_USE_STACK=0 -- unreachable in this build, kept
        // for completeness); any other nonzero = the first error line.
        if ( rc == -1 )
            log_->error() << "config: cannot open file: " << path << '\n';
        else if ( rc == -2 )
            log_->error() << "config: out of memory while parsing: " << path << '\n';
        else
            log_->error() << "config: rejected: " << path << " (line " << rc << ")\n";
        return false;
    }
    log_->info() << "config: loaded " << path << '\n';
    return true;
}
