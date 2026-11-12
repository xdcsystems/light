// The option registry: one table of names, ranges and error texts;
// both parameter sources (the argv loop and the INI config-file handler) 
// validate through it, so both reject identically.
// Owned by CliOptions; the config-file layer receives the registry by reference.

#pragma once
#include <array>
#include <ostream>
#include <string_view>

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

    struct Entry
    {
        const char* name;
        Option opt;
    };

    enum
    {
        kOptionCount = 21
    };

    OptionRegistry();

    const Option* find( std::string_view name ) const;
    void print_usage( std::ostream& os ) const;

private:
    std::array<Entry, kOptionCount> table_ {};
};
