// The option table, extracted from cli.cpp.
// One registry of names, ranges and error texts; both the
// argv loop (CliOptions::parse) and the INI config-file handler
// validate through it, so both sources reject identically.

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "config/option_registry.h"
#include "config/config.hpp"

namespace
{
    bool parse_int( const char* s, int* out )
    {
        if ( !s || !*s )
            return false;
        // strtol front-end kept over from_chars: from_chars rejects what
        // strtol accepted (leading whitespace, an explicit '+'), and
        // silently narrowing acceptance would change what "  10" / "+10"
        // mean. Overflow is caught by from_chars (result_out_of_range) --
        // no errno, no INT_MIN/INT_MAX compare. One deliberate edge: a
        // bare "+" was strtol-accepted as 0, it is rejected now.
        const char* ptr = s;
        while ( std::isspace( static_cast<unsigned char>( *ptr ) ) )
            ++ptr;
        if ( *ptr == '+' )
            ++ptr;
        const char* const last = s + std::strlen( s );
        int v = 0;
        const auto res = std::from_chars( ptr, last, v );
        if ( res.ec != std::errc{} || res.ptr != last )
            return false;
        *out = v;
        return true;
    }

    bool parse_float( const char* s, float* out )
    {
        if ( !s || !*s )
            return false;
        errno = 0;
        char* end = nullptr;
        const float v = std::strtof( s, &end );
        if ( errno == ERANGE || end == s || *end != '\0' || !std::isfinite( v ) )
            return false;
        *out = v;
        return true;
    }

    bool value_error( const char* opt, const char* val, const char* expected )
    {
        std::cerr << "invalid value for " << opt << ": '" << val << '\'';
        if ( expected )
            std::cerr << " (expected " << expected << ')';
        std::cerr << '\n';
        return false;
    }

    bool set_flag( bool& dst )
    {
        dst = true;
        return true;
    }

    bool set_str( const char* v, std::string& dst )
    {
        dst = v;
        return true;
    }

    bool set_int( const char* opt, const char* v, int lo, int hi, const char* exp, int& dst )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < lo || n > hi )
            return value_error( opt, v, exp );
        dst = n;
        return true;
    }

    bool set_float_01( const char* opt, const char* v, float& dst )
    {
        float f = NAN;
        if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
            return value_error( opt, v, "0.0..1.0" );
        dst = f;
        return true;
    }

    bool apply_cal_dir( Config& c, const char* v )
    {
        if ( !v || !*v )
            return value_error( "--cal-dir", v ? v : "", "non-empty path" );
        c.cal.dir = v;
        return true;
    }

    bool apply_device( Config& c, const char* v )
    {
        if ( !v || !*v )
            return value_error( "--device", v ? v : "", "/dev/videoN" );
        c.sources.push_back( { SourceKind::V4L2, v } );
        return true;
    }

    bool apply_config( Config& c, const char* )
    {
        ( void ) c;
        return true;
    }

    bool apply_core( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 2 && n != 3 && n != 4 && n != 7 ) )
            return value_error( "--core", v, "0|1|2|3|4|7" );
        c.infer.core = n;
        return true;
    }

    bool apply_net_src( Config& c, const char* v )
    {
        if ( !v || std::strncmp( v, "http://", 7 ) != 0 )
            return value_error( "--net-src", v ? v : "", "http://host:port/path" );
        c.sources.push_back( { SourceKind::NetMjpeg, v } );
        return true;
    }

    bool apply_pool( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 3 ) )
            return value_error( "--pool", v, "0|1|3" );
        c.infer.pool = n;
        return true;
    }
}  // namespace

OptionRegistry::OptionRegistry()
  : table_ { {
        { "--box-log",      { true, "per-frame box CSV for jitter A/B (raw vs smoothed)",
                                    +[]( Config& c, const char* v ) { return set_str( v, c.box_log ); } } },
        { "--cal",          { true, "calibration frames 1..10000",
                                    +[]( Config& c, const char* v ) { return set_int( "--cal", v, 1, 10000, "1..10000", c.cal.frames ); } } },
        { "--cal-dir",      { true, "directory for the --cal series (default: dataset/)", &apply_cal_dir } },
        { "--cal-interval", { true, "calibration sampling stride 1..10000",
                                    +[]( Config& c, const char* v ) { return set_int( "--cal-interval", v, 1, 10000, "1..10000", c.cal.interval ); } } },
        { "--conf",         { true, "confidence threshold 0.0..1.0",
                                    +[]( Config& c, const char* v ) { return set_float_01( "--conf", v, c.infer.conf_thr ); } } },
        { "--config",       { true, "INI config file (key = value); applied BEFORE the CLI options", &apply_config } },
        { "--core",         { true, "NPU cores: 0 auto, 1/2/4 = core 0/1/2, 3 (0+1), 7 (0+1+2); ignored with --pool > 1", &apply_core } },
        { "--cpu-draw",     { false, "draw boxes on CPU (A/B vs GPU drawer)",
                                    +[]( Config& c, const char* ) { return set_flag( c.stream.cpu_draw ); } } },
        { "--device",       { true, "camera device path (repeatable; default /dev/video20)", &apply_device } },
        { "--dump-cal",     { false, "dump one model-input frame to /tmp/cal.ppm",
                                    +[]( Config& c, const char* ) { return set_flag( c.cal.dump_one ); } } },
        { "--frames",       { true, "stop after N frames (>= 1; default: run until Ctrl-C)",
                                    +[]( Config& c, const char* v ) { return set_int( "--frames", v, 1, INT_MAX, ">= 1", c.frames ); } } },
        { "--iou",          { true, "NMS IoU threshold 0.0..1.0",
                                    +[]( Config& c, const char* v ) { return set_float_01( "--iou", v, c.infer.iou_thr ); } } },
        { "--model",        { true, "RKNN model path", +[]( Config& c, const char* v ) { return set_str( v, c.infer.model ); } } },
        { "--net",          { false, "network preview (raw rgb24 or --net-h264)",
                                    +[]( Config& c, const char* ) { return set_flag( c.stream.enabled ); } } },
        { "--net-bitrate",  { true, "preview bitrate 1000..100000 kbps",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-bitrate", v, 1000, 100000, "1000..100000 kbps", c.stream.bitrate_kbps ); } } },
        { "--net-fps",      { true, "preview stream fps 1..120, 0 = follow the source (VFR pts from dequeue times)",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-fps", v, 0, 120, "0..120", c.stream.fps ); } } },
        { "--net-h264",     { false, "H.264/MPEG-TS preview instead of raw rgb24",
                                    +[]( Config& c, const char* ) { return set_flag( c.stream.h264 ); } } },
        { "--net-host",     { true, "optional UDP destination (h264 push); unset = TCP pull",
                                    +[]( Config& c, const char* v ) { return set_str( v, c.stream.host ); } } },
        { "--net-port",     { true, "preview port 1..65535",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-port", v, 1, 65535, "1..65535", c.stream.port ); } } },
        { "--net-src",      { true, "network MJPEG pull source (http://host:port/path; repeatable)", &apply_net_src } },
        { "--pool",         { true, "NPU context pool: 0|1 = single context (baseline), 3 = one engine per core", &apply_pool } },
    } }
{
}

const OptionRegistry::Option* OptionRegistry::find( std::string_view name ) const
{
    // view compare, not strcmp: a string_view is not NUL-terminated
    // in general; the table names are literals, so == is exact.
    const auto *const it = std::find_if( table_.begin(), table_.end(),
                                  [name]( const Entry& e ) { return e.name == name; } );
    return it != table_.end() ? &it->opt : nullptr;
}

void OptionRegistry::print_usage( std::ostream& os ) const
{
    os << "usage: light_ai [options]\noptions:\n";
    for ( const auto& e : table_ )
    {
        const Option& o = e.opt;
        os << "  " << e.name << ( o.needs_value ? " <value>" : "" ) << " - " << o.desc << '\n';
    }
}
