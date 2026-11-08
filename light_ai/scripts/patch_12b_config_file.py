#!/usr/bin/env python3
# patch_12b_config_file.py (2026-11-08): move the INI config-file layer
# out of cli.cpp into include/config_file.hpp + src/config_file.cpp.
# The handler, its context, key normalization and flag parsing become
# local helpers of the new TU; cli.cpp keeps only the CliOptions
# orchestration (no anonymous namespace left). CMakeLists gains
# src/config_file.cpp. cli.hpp needs no changes.
#
# Usage:  python3 scripts/patch_12b_config_file.py   (project root)

import sys
from pathlib import Path

CLI_CPP = Path("src/cli.cpp")
CMAKE = Path("CMakeLists.txt")
CFG_H = Path("include/config_file.hpp")
CFG_CPP = Path("src/config_file.cpp")


def die(msg):
    print("PATCH ABORTED:", msg)
    sys.exit(1)


def must_replace(text, old, new, label):
    n = text.count(old)
    if n != 1:
        die(label + ": expected exactly 1 occurrence, found " + str(n))
    return text.replace(old, new, 1)


CFG_H_TEXT = r'''// config_file.hpp (2026-11-08): the INI config-file layer (externals/
// inih r62, BSD-3-Clause). Loads a flat `key = value` file THROUGH the
// option registry the CLI uses, so both sources validate identically
// and print the same error texts. The load happens in the parse()
// pre-pass, before the argv loop: defaults < config file < CLI.

#pragma once
#include <string>

struct Config;
class OptionRegistry;

class ConfigFileLoader
{
public:
    explicit ConfigFileLoader( const OptionRegistry& reg );

    bool load( const char* path, Config& cfg ) const;

private:
    const OptionRegistry* reg_;
};
'''

CFG_CPP_TEXT = r'''// config_file.cpp (2026-11-08): the INI config-file layer. inih parses
// a flat `key = value` file THROUGH the option registry the CLI uses
// -- one registry, one validation path, identical error text in both
// sources. Flat namespace only: a section header is a loud error (the
// handler is invoked at every section start via
// INI_CALL_HANDLER_ON_NEW_SECTION=1, set in CMakeLists.txt).

#include <cctype>
#include <iostream>
#include <string>

#include "cli.hpp"
#include "config_file.hpp"
#include "ini.h"
#include "option_registry.hpp"

namespace
{
    // HANDLER CONVENTION: 0 = error, nonzero = success (per ini.h:
    // "nonzero on success, zero on error") -- the returns below follow
    // it; the 2026-11-08 bug had them inverted.
    struct CfgFileCtx
    {
        Config* cfg;
        const OptionRegistry* reg;
    };

    // Normalization: leading dashes stripped, '_' -> '-', then the
    // "--" prefix re-attached -- both `net_h264 = true` and
    // `--net-h264 = true` land on the same table row.
    std::string normalize_key( const char* key )
    {
        std::string norm;
        for ( const char* p = key; *p; ++p )
        {
            if ( *p == '-' && norm.empty() )
                continue;
            norm += ( *p == '_' ) ? '-' : *p;
        }
        return norm;
    }

    // Flags default to off and are only ever set: the file may
    // turn them on (true/yes/on/1, case-insensitive), never off.
    bool flag_truthy( const char* value )
    {
        std::string v = value ? value : "";
        for ( char& ch : v )
            ch = static_cast<char>( std::tolower( static_cast<unsigned char>( ch ) ) );
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
            std::cerr << "config: sections are not supported: [" << section
                      << "]; flat key = value only\n";
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
            if ( !flag_truthy( value ) )
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
            // parsing continues) and is reported by load().
            std::cerr << "config: rejected value (key '" << key << "')\n";
            return 0;
        }
        return 1;
    }
}  // namespace

ConfigFileLoader::ConfigFileLoader( const OptionRegistry& reg )
    : reg_( &reg )
{
}

bool ConfigFileLoader::load( const char* path, Config& cfg ) const
{
    CfgFileCtx cx { &cfg, reg_ };
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
'''

FIXES_CPP = [
    ("file header comment",
     r'''// cli.cpp (2026-11-08): the parameter layer. The option registry
// (names, ranges, error texts) lives in option_registry.{hpp,cpp};
// this file owns the INI config-file layer (externals/inih r62,
// BSD-3-Clause) and the CliOptions orchestration (pre-pass, argv
// loop, cross-field checks).
// Precedence: defaults < config file < CLI.
''',
     r'''// cli.cpp (2026-11-08): the parameter layer. The option registry
// (names, ranges, error texts) lives in option_registry.{hpp,cpp};
// the INI config-file layer lives in config_file.{hpp,cpp}
// (externals/inih r62, BSD-3-Clause); this file owns the CliOptions
// orchestration (pre-pass, argv loop, cross-field checks).
// Precedence: defaults < config file < CLI.
'''),

    ("include block",
     r'''#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

#include "cli.hpp"
#include "ini.h"
''',
     r'''#include <cstring>
#include <iostream>
#include <string>

#include "cli.hpp"
#include "config_file.hpp"
'''),

    ("parse: config-file load",
     r'''    if ( !config_path.empty() && !load_config_file( config_path.c_str(), cfg_, reg_ ) )
        return ParseStatus::Error;
''',
     r'''    if ( !config_path.empty() )
    {
        ConfigFileLoader loader( reg_ );
        if ( !loader.load( config_path.c_str(), cfg_ ) )
            return ParseStatus::Error;
    }
'''),
]

FIXES_CMAKE = [
    ("CMake: config_file.cpp",
     "    src/option_registry.cpp\n",
     "    src/option_registry.cpp\n    src/config_file.cpp\n"),
]


def main():
    for p in (CLI_CPP, CMAKE):
        if not p.exists():
            die(str(p) + " not found -- run from the project root")
    for p in (CFG_H, CFG_CPP):
        if p.exists():
            die(str(p) + " already exists")

    cpp = CLI_CPP.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")

    for label, old, new in FIXES_CPP:
        cpp = must_replace(cpp, old, new, label)
    for label, old, new in FIXES_CMAKE:
        cmake = must_replace(cmake, old, new, label)

    # Indices AFTER the replacements (2026-11-08 lesson: replacements
    # shrink the text, stale indices splice mid-line).
    NS_OPEN = "namespace\n{\n"
    NS_OPEN_N = cpp.count(NS_OPEN)
    if NS_OPEN_N != 1:
        die("anonymous namespace open anchor: expected exactly 1, found "
            + str(NS_OPEN_N))
    IMPL_MARK = "// ---- CliOptions implementation ----"
    if cpp.count(IMPL_MARK) != 1:
        die("implementation marker: expected exactly 1, found "
            + str(cpp.count(IMPL_MARK)))
    i0 = cpp.index(NS_OPEN)
    i1 = cpp.index(IMPL_MARK)
    if i1 <= i0:
        die("splice anchors are out of order")
    cpp = cpp[:i0] + cpp[i1:]

    # Read-back: cli.cpp -- absence of the moved layer, presence of the
    # loader, integrity of the kept region.
    for needle in ("CfgFileCtx", "cfg_file_handler", "load_config_file",
                   "ini_parse", "ini.h", "cctype", "namespace"):
        if needle in cpp:
            die("read-back (cli.cpp): '" + needle + "' remains -- the splice "
                "cut is misaligned or the file was edited")
    if "ConfigFileLoader loader( reg_ )" not in cpp:
        die("read-back (cli.cpp): the loader construction is missing")
    if "loader.load( config_path.c_str(), cfg_ )" not in cpp:
        die("read-back (cli.cpp): the loader call is missing")
    if cpp.count("CliOptions::validate() const") != 1:
        die("read-back (cli.cpp): validate() damaged")
    if cpp.count("return 2;") != 3:
        die("read-back (cli.cpp): the cross-field checks are damaged ("
            + str(cpp.count("return 2;")) + " of 3 loud rejections)")
    if "Flat namespace only" in cpp:
        die("read-back (cli.cpp): the config-file layer comment remains")

    # Read-back: the new TU carries the whole layer, verbatim anchors.
    if CFG_CPP_TEXT.count("int cfg_file_handler( void* user, const char* section, const char* key,") != 1:
        die("read-back (config_file.cpp): the handler definition is missing")
    if CFG_CPP_TEXT.count("struct CfgFileCtx") != 1:
        die("read-back (config_file.cpp): CfgFileCtx duplicated")
    if CFG_CPP_TEXT.count("bool ConfigFileLoader::load( const char* path, Config& cfg ) const") != 1:
        die("read-back (config_file.cpp): the load definition is missing")
    if CFG_CPP_TEXT.count("ConfigFileLoader::ConfigFileLoader") != 1:
        die("read-back (config_file.cpp): the ctor definition is missing")
    if CFG_CPP_TEXT.count("std::string normalize_key( const char* key )") != 1:
        die("read-back (config_file.cpp): normalize_key definition missing")
    if CFG_CPP_TEXT.count("bool flag_truthy( const char* value )") != 1:
        die("read-back (config_file.cpp): flag_truthy definition missing")
    if CFG_CPP_TEXT.count("return 0;") != 7:
        die("read-back (config_file.cpp): handler error paths damaged ("
            + str(CFG_CPP_TEXT.count("return 0;")) + " of 7)")
    if CFG_CPP_TEXT.count("Flat namespace only") != 1:
        die("read-back (config_file.cpp): the flat-namespace note is missing")
    if CFG_H_TEXT.count("class ConfigFileLoader") != 1:
        die("read-back (config_file.hpp): the class is missing")
    if CFG_H_TEXT.count("const OptionRegistry* reg_;") != 1:
        die("read-back (config_file.hpp): the member is missing")
    if "    src/config_file.cpp\n" not in cmake:
        die("read-back (CMakeLists): config_file.cpp missing")

    CLI_CPP.with_suffix(".cpp.bak").write_text(CLI_CPP.read_text(encoding="utf-8"),
                                               encoding="utf-8")
    CMAKE.with_suffix(".txt.bak").write_text(cmake, encoding="utf-8")

    CFG_H.write_text(CFG_H_TEXT, encoding="utf-8")
    CFG_CPP.write_text(CFG_CPP_TEXT, encoding="utf-8")
    CLI_CPP.write_text(cpp, encoding="utf-8")
    CMAKE.write_text(cmake, encoding="utf-8")

    print("--- new: include/config_file.hpp, src/config_file.cpp")
    print("--- src/cli.cpp: the INI layer moved out, " + str(len(FIXES_CPP))
          + " replacements (backup: cli.cpp.bak)")
    print("--- CMakeLists.txt: src/config_file.cpp added (backup: CMakeLists.txt.bak)")
    print("OK: full gate re-run: ./scripts/build.sh (WITHOUT SKIP_TIDY)")


if __name__ == "__main__":
    main()

