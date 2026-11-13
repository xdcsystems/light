// Minimal harness for the host-side unit tests (light_ai_tests).
// Project rules hold here too: no manual heap, no statics, no virtuals, iostream only.
// Usage:
//   CHECK( t, cond )            - counts the check, logs file:line on failure;
//   CHECK_NEAR( t, a, b, eps )  - |a - b| <= eps, for float results;
//   QuietStreams q;             - mutes std::cout/std::cerr for the scope
//                                 (error-path tests print loud rejections by design).

#pragma once

#include <cmath>
#include <iostream>
#include <sstream>

class TestRun
{
public:
    void check( bool cond, const char* expr, const char* file, int line )
    {
        ++checks_;
        if ( cond )
            return;
        ++failures_;
        std::cerr << file << ':' << line << ": FAILED: " << expr << '\n';
    }
    int checks() const { return checks_; }
    int failures() const { return failures_; }

private:
    int checks_ = 0;
    int failures_ = 0;
};

class QuietStreams
{
public:
    QuietStreams()
      : old_err_( std::cerr.rdbuf( sink_.rdbuf() ) )
      , old_out_( std::cout.rdbuf( sink_.rdbuf() ) )
    {
    }
    ~QuietStreams()
    {
        std::cerr.rdbuf( old_err_ );
        std::cout.rdbuf( old_out_ );
    }
    QuietStreams( const QuietStreams& ) = delete;
    QuietStreams& operator=( const QuietStreams& ) = delete;

private:
    std::ostringstream sink_;
    std::streambuf* old_err_;
    std::streambuf* old_out_;
};

#define CHECK( t, cond ) ( t ).check( ( cond ), #cond, __FILE__, __LINE__ )
#define CHECK_NEAR( t, a, b, eps ) ( t ).check( std::fabs( ( a ) - ( b ) ) <= ( eps ), #a " ~= " #b, __FILE__, __LINE__ )
