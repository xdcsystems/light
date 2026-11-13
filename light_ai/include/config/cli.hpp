#pragma once

#include "config/config.hpp"  // Config and its sections
#include "config/option_registry.h"

// CliOptions: the single place that reads and validates the command
// line and the INI config file (externals/inih, BSD-3-Clause). One
// registry of names, ranges and error texts (OptionRegistry);
// the config-file layer reuses the same registry, so both sources
// validate identically. parse() is called once from main();
// afterwards config() hands over a frozen, validated Config.
class CliOptions
{
public:
    enum class ParseStatus
    {
        Run,    // argv + config file validated; proceed to the Pipeline
        Help,   // --help/-h printed; main() exits 0
        Error,  // loud rejection; main() exits 2
    };

    ParseStatus parse( int argc, char** argv );
    const Config& config() const { return cfg_; }

private:
    int validate() const;  // cross-field checks; 0 = ok, 2 = loud rejection

private:
    OptionRegistry reg_;
    Config cfg_;
};
