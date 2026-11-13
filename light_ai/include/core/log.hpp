// Log: the diagnostic output of the process, injected by reference instead
// of reaching for std::cout / std::cerr directly.
//
// Why a class: a component that writes to the global streams cannot be
// silenced, redirected or checked in a test. Log is owned by main() and
// passed down as `const Log&`; a test hands in two std::ostringstream.
//
// Usage is a drop-in for the stream it replaces -- the line is built with
// operator<< and ends with the caller's own '\n':
//     log_.info() << "source: geometry " << w << 'x' << h << '\n';
//     log_.warn() << "probe s" << idx << ": start failed\n";
// info()/debug() go to the `out` stream, warn()/error() to the `err`
// stream (std::cout / std::cerr in production: the same destinations and
// buffering as before).
//
// Each call returns a Line that holds the log mutex until the end of the
// full expression, so one statement is one uninterrupted line even with
// several threads logging. The mutex is recursive: an operand that logs
// itself nests instead of deadlocking. A Line below the threshold writes
// nothing, takes no lock and still evaluates its operands.
//
// Not for per-pixel or per-detection paths: a Line costs a mutex.
// No heap, no statics, no virtuals; SDK-free.

#pragma once

#include <cstdint>
#include <ios>
#include <mutex>
#include <ostream>

class Log
{
public:
    enum class Level : uint8_t
    {
        Debug,
        Info,
        Warn,
        Error,
        Off  // threshold only: silences everything
    };

    // One statement's worth of output. Created only by Log; lives until the
    // end of the full expression (guaranteed copy elision: never copied or moved).
    class Line
    {
    public:
        // os == nullptr: the line is below the threshold -- no lock, no output.
        Line( std::ostream* os, std::recursive_mutex& mx ) : os_( os )
        {
            if ( os_ )
                lk_ = std::unique_lock<std::recursive_mutex>( mx );
        }
        Line( const Line& ) = delete;
        Line& operator=( const Line& ) = delete;
        Line( Line&& ) = delete;
        Line& operator=( Line&& ) = delete;
        ~Line() = default;

        template <typename T>
        Line& operator<<( const T& v )
        {
            if ( os_ )
                *os_ << v;
            return *this;
        }
        // Stream manipulators (std::hex, std::dec, std::fixed, ...).
        Line& operator<<( std::ios_base& ( *manip )( std::ios_base& ) )
        {
            if ( os_ )
                *os_ << manip;
            return *this;
        }
        Line& operator<<( std::ostream& ( *manip )( std::ostream& ) )
        {
            if ( os_ )
                *os_ << manip;
            return *this;
        }

    private:
        std::ostream* os_;
        std::unique_lock<std::recursive_mutex> lk_;
    };

    // out/err must outlive the Log. min_level: lines below it are dropped.
    Log( std::ostream& out, std::ostream& err, Level min_level = Level::Info ) : out_( out ), err_( err ), min_( min_level ) {}
    Log( const Log& ) = delete;
    Log& operator=( const Log& ) = delete;
    Log( Log&& ) = delete;
    Log& operator=( Log&& ) = delete;
    ~Log() = default;

    Line debug() const { return line( Level::Debug, out_ ); }
    Line info() const { return line( Level::Info, out_ ); }
    Line warn() const { return line( Level::Warn, err_ ); }
    Line error() const { return line( Level::Error, err_ ); }

    bool enabled( Level level ) const { return level >= min_; }

private:
    Line line( Level level, std::ostream& os ) const { return { enabled( level ) ? &os : nullptr, mx_ }; }

    std::ostream& out_;
    std::ostream& err_;
    const Level min_;
    mutable std::recursive_mutex mx_;  // mutable: logging through a const Log& is the normal use
};
