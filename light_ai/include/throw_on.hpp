// throw_on: the one bridge from a Status to an exception, for the startup path
// only (README, "Rules of the project", Errors: exceptions only at startup).
// The text is "<context>: <status>"; main() prints it as "Fatal: <text>".
// Not in core/: core/ code never throws.

#pragma once

#include <sstream>
#include <stdexcept>

#include "core/status.hpp"

inline void throw_on( const Status& st, const char* context )
{
    if ( st.ok() )
        return;
    std::ostringstream os;
    os << context << ": " << st;
    throw std::runtime_error( os.str() );
}
