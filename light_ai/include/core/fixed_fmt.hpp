// Fixed: one floating-point value inserted as std::fixed with `prec`
// digits after the point; the stream's flags and precision are restored
// right after it. Log shares std::cout with every other writer, so a
// format set for one value must not leak into the next line:
//     log_.info() << "wall " << Fixed { wall, 3 } << " s\n";
// The value goes through the stream's own num_put, so the text is exactly
// what `os << std::fixed << std::setprecision( prec ) << v` prints.
// SDK-free, no state.
#pragma once

#include <iomanip>
#include <ios>
#include <ostream>

struct Fixed
{
    double v;
    int prec;
};

inline std::ostream& operator<<( std::ostream& os, Fixed f )
{
    const std::ios_base::fmtflags old_flags = os.flags();
    const std::streamsize old_prec = os.precision();
    os << std::fixed << std::setprecision( f.prec ) << f.v;
    os.flags( old_flags );
    os.precision( old_prec );
    return os;
}
