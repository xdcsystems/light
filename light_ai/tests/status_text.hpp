// Test helper: a Status as the text the log would print (operator<<).

#pragma once

#include <sstream>
#include <string>

#include "core/status.hpp"

inline std::string status_text( const Status& s )
{
    std::ostringstream os;
    os << s;
    return os.str();
}
