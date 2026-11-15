#pragma once

#include <ostream>
#include <string>

#include "config/config.hpp"  // Config and its sections
#include "config/option_registry.h"
#include "core/log.hpp"

// CliOptions: the single place that reads and validates the command
// line and the INI config file (externals/inih, BSD-3-Clause). One
// registry of names, ranges and error texts (OptionRegistry);
// the config-file layer reuses the same registry, so both sources
// validate identically. parse() is called once from main();
// afterwards config() hands over a frozen, validated Config.
// Rejections go to log.error(), "config: loaded" to log.info(); the
// usage text is printed by main() (print_usage) on ParseStatus::Help.
class CliOptions
{
public:
    enum class ParseStatus
    {
        Run,    // argv + config file validated; proceed to the Pipeline
        Help,   // --help/-h seen; main() prints the usage and exits 0
        Error,  // loud rejection; main() exits 2
    };

    explicit CliOptions( const Log& log );
    ParseStatus parse( int argc, char** argv );
    const Config& config() const { return cfg_; }
    void print_usage( std::ostream& os ) const { reg_.print_usage( os ); }

private:
    // The --config / --help pre-pass: Help, Error (--config without a
    // value) or Run with config_path set when the file is named.
    ParseStatus prepass( int argc, char** argv, std::string& config_path ) const;
    bool apply_argv( int argc, char** argv );  // the option loop; false = loud rejection
    int validate() const;  // cross-field checks; 0 = ok, 2 = loud rejection

private:
    OptionRegistry reg_;
    Config cfg_;
    const Log& log_;
};
