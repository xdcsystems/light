// The INI config-file layer (externals/inih r62, BSD-3-Clause).
// Loads a flat `key = value` file THROUGH the option registry the
// CLI uses, so both sources validate identically and print the same
// error texts. The load happens in the parse() pre-pass, before the
// argv loop: defaults < config file < CLI.

#pragma once
#include <string>

#include "core/log.hpp"

struct Config;
class OptionRegistry;

class ConfigFileLoader
{
public:
    ConfigFileLoader( const OptionRegistry& reg, const Log& log );

    bool load( const char* path, Config& cfg ) const;

private:
    const OptionRegistry* reg_;
    const Log* log_;
};
