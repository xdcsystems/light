// option_registry.h (2026-11-08): the option registry. One table of
// names, ranges and error texts; both parameter sources (the argv loop
// and the INI config-file handler) validate through it, so both reject
// identically. Owned by CliOptions; the config-file layer receives the
// registry by reference.

#pragma once
#include <map>
#include <ostream>
#include <string>

struct Config;

class OptionRegistry
{
public:
    struct Option
    {
        bool needs_value;
        const char* desc;
        bool ( *apply )( Config& cfg, const char* val );
    };

    OptionRegistry();

    const Option* find( const char* name ) const;
    void print_usage( std::ostream& os ) const;

private:
    std::map<std::string, Option> table_;
};
