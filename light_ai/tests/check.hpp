// Minimal harness for the host-side unit tests (light_ai_tests).
// Project rules hold here too: no manual heap, no statics, no virtuals, iostream only.
// Usage:
//   CHECK( t, cond )            - counts the check, logs file:line and the expression on failure;
//   CHECK_EQ( t, a, b )         - a == b; on failure also prints both values (operator<<);
//   CHECK_STR_EQ( t, a, b )     - std::string / const char* equality; on failure prints both texts
//                                 quoted and escaped (\n, \t, ...) and the index of the first difference;
//   CHECK_NEAR( t, a, b, eps )  - |a - b| <= eps, for float results; on failure prints both values and |a - b|;
// Every macro argument is evaluated exactly once. CHECK_EQ compares const A& with const B&: pass matching
// types (int vs size_t warns with -Wsign-compare inside the template). A type with no operator<< stays CHECK.
// A TestRun reports to the stream it is given (std::cerr by default; the harness self-test passes an ostringstream).

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace check_detail
{
    // One character of a quoted text: the C escapes, other control bytes as \xNN.
    inline void put_escaped( std::ostream& os, char c )
    {
        switch ( c )
        {
            case '\n': os << "\\n"; return;
            case '\t': os << "\\t"; return;
            case '\r': os << "\\r"; return;
            case '\\': os << "\\\\"; return;
            case '"': os << "\\\""; return;
            default: break;
        }
        const auto u = static_cast<unsigned char>( c );
        if ( u >= 0x20 && u != 0x7f )
        {
            os << c;
            return;
        }
        char hex[8] = {};
        std::snprintf( hex, sizeof hex, "\\x%02x", static_cast<unsigned>( u ) );
        os << hex;
    }

    inline void put_quoted( std::ostream& os, std::string_view s )
    {
        os << '"';
        for ( const char c : s )
            put_escaped( os, c );
        os << '"';
    }

    // A value as CHECK_EQ / CHECK_NEAR print it: bool as true/false, (un)signed char (uint8_t) as a number,
    // a plain char quoted, an enum as its underlying value, floating point with every significant digit.
    template <typename T>
    void put_value( std::ostream& os, const T& v )
    {
        if constexpr ( std::is_enum_v<T> )
            put_value( os, static_cast<std::underlying_type_t<T>>( v ) );
        else if constexpr ( std::is_same_v<T, bool> )
            os << ( v ? "true" : "false" );
        else if constexpr ( std::is_same_v<T, char> )
        {
            os << '\'';
            put_escaped( os, v );
            os << '\'';
        }
        else if constexpr ( std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> )
            os << static_cast<int>( v );
        else if constexpr ( std::is_floating_point_v<T> )
            os << std::setprecision( std::numeric_limits<T>::max_digits10 ) << v;
        else
            os << v;
    }

    // CHECK_STR_EQ's view of one side; a null const char* is kept apart from "" and never equals anything.
    struct StrArg
    {
        std::string_view text;
        bool null = false;
    };
    inline StrArg str_arg( const char* s ) { return s != nullptr ? StrArg { s, false } : StrArg { {}, true }; }
    inline StrArg str_arg( std::string_view s ) { return StrArg { s, false }; }

    inline void put_str_arg( std::ostream& os, const StrArg& s )
    {
        if ( s.null )
            os << "(null)";
        else
            put_quoted( os, s.text );
    }

    // Index of the first differing character; the shorter length when one text is a prefix of the other.
    inline std::size_t first_difference( std::string_view a, std::string_view b )
    {
        std::size_t i = 0;
        while ( i < a.size() && i < b.size() && a[i] == b[i] )
            ++i;
        return i;
    }

    inline std::ostream& head( std::ostream& m, const char* file, int line )
    {
        return m << file << ':' << line << ": FAILED: ";
    }

    template <typename T>
    constexpr bool kIsCharPointer = std::is_pointer_v<std::decay_t<T>> &&
            std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char>;
}  // namespace check_detail

class TestRun
{
public:
    explicit TestRun( std::ostream& out = std::cerr )
      : out_( out )
    {
    }

    void check( bool cond, const char* expr, const char* file, int line )
    {
        if ( count( cond ) )
            return;
        std::ostringstream m;
        check_detail::head( m, file, line ) << expr << '\n';
        out_ << m.str();
    }

    template <typename A, typename B>
    void check_eq( const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line )
    {
        static_assert( !check_detail::kIsCharPointer<A> && !check_detail::kIsCharPointer<B>,
                "CHECK_EQ on C strings compares pointers: use CHECK_STR_EQ" );
        if ( count( static_cast<bool>( a == b ) ) )
            return;
        std::ostringstream m;
        check_detail::head( m, file, line ) << expr_a << " == " << expr_b << "\n  left:  ";
        check_detail::put_value( m, a );
        m << "\n  right: ";
        check_detail::put_value( m, b );
        m << '\n';
        out_ << m.str();
    }

    void check_str_eq( const check_detail::StrArg& a, const check_detail::StrArg& b, const char* expr_a, const char* expr_b,
            const char* file, int line )
    {
        if ( count( !a.null && !b.null && a.text == b.text ) )
            return;
        std::ostringstream m;
        check_detail::head( m, file, line ) << expr_a << " == " << expr_b << "\n  left:  ";
        check_detail::put_str_arg( m, a );
        m << "\n  right: ";
        check_detail::put_str_arg( m, b );
        m << '\n';
        if ( !a.null && !b.null )
            m << "  first difference at index " << check_detail::first_difference( a.text, b.text ) << '\n';
        out_ << m.str();
    }

    template <typename A, typename B, typename E>
    void check_near( const A& a, const B& b, const E& eps, const char* expr_a, const char* expr_b, const char* expr_eps,
            const char* file, int line )
    {
        const auto diff = std::fabs( a - b );
        if ( count( diff <= eps ) )
            return;
        std::ostringstream m;
        check_detail::head( m, file, line ) << expr_a << " ~= " << expr_b << " (eps " << expr_eps << ")\n  left:  ";
        check_detail::put_value( m, a );
        m << "\n  right: ";
        check_detail::put_value( m, b );
        m << "\n  diff:  ";
        check_detail::put_value( m, diff );
        m << '\n';
        out_ << m.str();
    }

    int checks() const { return checks_; }
    int failures() const { return failures_; }

private:
    bool count( bool ok )
    {
        ++checks_;
        if ( !ok )
            ++failures_;
        return ok;
    }

private:
    std::ostream& out_;
    int checks_ = 0;
    int failures_ = 0;
};

#define CHECK( t, cond ) ( t ).check( ( cond ), #cond, __FILE__, __LINE__ )
#define CHECK_EQ( t, a, b ) ( t ).check_eq( ( a ), ( b ), #a, #b, __FILE__, __LINE__ )
#define CHECK_STR_EQ( t, a, b ) \
    ( t ).check_str_eq( check_detail::str_arg( a ), check_detail::str_arg( b ), #a, #b, __FILE__, __LINE__ )
#define CHECK_NEAR( t, a, b, eps ) ( t ).check_near( ( a ), ( b ), ( eps ), #a, #b, #eps, __FILE__, __LINE__ )
