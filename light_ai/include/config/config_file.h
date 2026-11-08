// config_file.h (2026-11-08): the INI config-file layer (externals/
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
