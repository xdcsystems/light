// The option registry: one table of names, ranges and error texts;
// both parameter sources (the argv loop and the INI config-file handler) 
// validate through it, so both reject identically.
// Owned by CliOptions; the config-file layer receives the registry by reference.
// The apply functions only parse and store; a rejected value is reported
// once, by apply() below, with the row's `expected` text through Log.

#pragma once
#include <array>
#include <ostream>
#include <string_view>

#include "core/log.hpp"

struct Config;

class OptionRegistry
{
public:
    struct Option
    {
        bool needs_value;
        const char* desc;
        const char* expected;  // the accepted values, for the rejection text; nullptr: apply never rejects
        bool ( *apply )( Config& cfg, const char* val );  // false: malformed / out of range, nothing stored
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

    explicit OptionRegistry( const Log& log );

    const Entry* find( std::string_view name ) const;
    // Runs the row's apply on val (nullptr for a flag). On a rejection logs
    // "invalid value for <name>: '<val>' (expected <expected>)" and returns false.
    bool apply( const Entry& e, Config& cfg, const char* val ) const;
    void print_usage( std::ostream& os ) const;

private:
    std::array<Entry, kOptionCount> table_ {};
    const Log& log_;
};
