// core/fixed_fmt.hpp: Fixed prints std::fixed with prec digits and restores
// the stream's flags and precision (Log shares std::cout with every writer).

#include <ios>
#include <sstream>

#include "core/fixed_fmt.hpp"
#include "check.hpp"
#include "suites.hpp"

void test_fixed_fmt( TestRun& t )
{
    {
        std::ostringstream os;
        os << Fixed { 3.14159, 2 } << ' ' << Fixed { 2.5, 0 } << ' ' << Fixed { -0.04, 1 } << ' ' << Fixed { 10.0, 3 };
        CHECK_STR_EQ( t, os.str(), "3.14 2 -0.0 10.000" );  // round-half-even of 2.5 at prec 0 is the num_put result
    }
    // Flags and precision restored: the next plain value prints as before.
    {
        std::ostringstream os;
        os.precision( 4 );
        os << std::hex;
        const std::ios_base::fmtflags before = os.flags();
        os << Fixed { 1.0 / 3.0, 5 };
        CHECK( t, os.flags() == before && os.precision() == 4 );
        os << ' ' << 255 << ' ' << 0.123456;
        CHECK_STR_EQ( t, os.str(), "0.33333 ff 0.1235" );
    }
    // Large and tiny magnitudes stay fixed (no exponent).
    {
        std::ostringstream os;
        os << Fixed { 1.0e7, 1 } << ' ' << Fixed { 1.0e-7, 3 };
        CHECK_STR_EQ( t, os.str(), "10000000.0 0.000" );
    }
}
