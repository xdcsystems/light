// option_registry.cpp (2026-11-08): the option table, extracted from
// cli.cpp. One registry of names, ranges and error texts; both the
// argv loop (CliOptions::parse) and the INI config-file handler
// validate through it, so both sources reject identically.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "config/option_registry.h"
#include "config/cli.hpp"

namespace
{
    bool parse_int( const char* s, int* out )
    {
        if ( !s || !*s )
            return false;
        errno = 0;
        char* end = nullptr;
        const long v = std::strtol( s, &end, 10 );
        if ( errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX )
            return false;
        *out = static_cast<int>( v );
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

    template <bool Config::*Field>
    bool apply_flag( Config& c, const char* )
    {
        c.*Field = true;
        return true;
    }

    template <std::string Config::*Field>
    bool apply_str( Config& c, const char* v )
    {
        c.*Field = v;
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
        std::error_code ec;
        std::filesystem::create_directories( v, ec );
        if ( ec )
        {
            std::cerr << "invalid value for --cal-dir: '" << v << "' (" << ec.message() << ")\n";
            return false;
        }
        const std::string probe = std::string( v ) + "/.light_ai_write_test";
        bool ok = false;
        {
            std::ofstream f( probe );
            ok = f.good();
        }
        std::filesystem::remove( probe, ec );
        if ( !ok )
        {
            std::cerr << "invalid value for --cal-dir: '" << v << "' (not writable)\n";
            return false;
        }
        c.cal_dir = v;
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
        c.core = n;
        return true;
    }

    bool apply_net_src( Config& c, const char* v )
    {
        if ( std::strncmp( v, "http://", 7 ) != 0 )
            return value_error( "--net-src", v, "http://host:port/path" );
        c.net_src = v;
        c.device.clear();
        return true;
    }

    bool apply_pool( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 3 ) )
            return value_error( "--pool", v, "0|1|3" );
        c.pool = n;
        return true;
    }

    bool apply_dim( const char* opt, const char* v, int& dst )
    {
        if ( !set_int( opt, v, 16, 8192, "16..8192", dst ) )
            return false;
        if ( dst % 16 != 0 )
            std::cerr << "warning: " << opt << " " << dst << " is not a multiple of 16: some UVC cameras reject it\n";
        return true;
    }
}  // namespace

OptionRegistry::OptionRegistry()
  : table_ { {
        { "--box-log",      { true, "per-frame box CSV for jitter A/B (raw vs smoothed)", &apply_str<&Config::box_log> } },
        { "--cal",          { true, "calibration frames 1..10000",
                                     +[]( Config& c, const char* v ) { return set_int( "--cal", v, 1, 10000, "1..10000", c.cal_frames ); } } },
        { "--cal-dir",      { true, "directory for the --cal series (default: dataset/)", &apply_cal_dir } },
        { "--cal-interval", { true, "calibration sampling stride 1..10000",
                                    +[]( Config& c, const char* v ) { return set_int( "--cal-interval", v, 1, 10000, "1..10000", c.cal_interval ); } } },
        { "--conf",         { true, "confidence threshold 0.0..1.0",
                                    +[]( Config& c, const char* v ) { return set_float_01( "--conf", v, c.conf_thr ); } } },
        { "--config",       { true, "INI config file (key = value); applied BEFORE the CLI options", &apply_config } },
        { "--core",         { true, "NPU cores: 0 auto, 1/2/4 = core 0/1/2, 3 (0+1), 7 (0+1+2); ignored with --pool > 1", &apply_core } },
        { "--cpu-draw",     { false, "draw boxes on CPU (A/B vs GPU drawer)", &apply_flag<&Config::cpu_draw> } },
        { "--device",       { true, "camera device path", &apply_str<&Config::device> } },
        { "--dump-cal",     { false, "collect calibration dataset", &apply_flag<&Config::dump_cal> } },
        { "--frames",       { true, "stop after N frames (>= 1; default: run until Ctrl-C)",
                                    +[]( Config& c, const char* v ) { return set_int( "--frames", v, 1, INT_MAX, ">= 1", c.frames ); } } },
        { "--h",            { true, "camera height", +[]( Config& c, const char* v ) { return apply_dim( "--h", v, c.cam_h ); } } },
        { "--iou",          { true, "NMS IoU threshold 0.0..1.0",
                                    +[]( Config& c, const char* v ) { return set_float_01( "--iou", v, c.iou_thr ); } } },
        { "--model",        { true, "RKNN model path", &apply_str<&Config::model> } },
        { "--net",          { false, "network preview (raw rgb24 or --net-h264)", &apply_flag<&Config::net> } },
        { "--net-bitrate",  { true, "preview bitrate 1000..100000 kbps",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-bitrate", v, 1000, 100000, "1000..100000 kbps", c.net_bitrate ); } } },
        { "--net-fps",      { true, "preview stream fps 1..120, 0 = follow the source (VFR pts from dequeue times)",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-fps", v, 0, 120, "0..120", c.net_fps ); } } },
        { "--net-h264",     { false, "H.264/MPEG-TS preview instead of raw rgb24", &apply_flag<&Config::net_h264> } },
        { "--net-host",     { true, "optional UDP destination (h264 push); unset = TCP pull", &apply_str<&Config::net_host> } },
        { "--net-port",     { true, "preview port 1..65535",
                                    +[]( Config& c, const char* v ) { return set_int( "--net-port", v, 1, 65535, "1..65535", c.net_port ); } } },
        { "--net-src",      { true, "network MJPEG pull source (http://host:port/path); replaces --device", &apply_net_src } },
        { "--pool",         { true, "NPU context pool: 1 = single context (baseline), 3 = one engine per core", &apply_pool } },
        { "--w",            { true, "camera width", +[]( Config& c, const char* v ) { return apply_dim( "--w", v, c.cam_w ); } } },
    } }
{
}

const OptionRegistry::Option* OptionRegistry::find( const char* name ) const
{
    for ( const auto& e : table_ )
    {
        if ( std::strcmp( e.name, name ) == 0 )
            return &e.opt;
    }
    return nullptr;
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
