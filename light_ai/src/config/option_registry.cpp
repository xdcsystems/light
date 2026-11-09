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
        {
            return false;
        }
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
        if ( errno == ERANGE || end == s || *end != '\0' )
            return false;
        if ( !std::isfinite( v ) )
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

    bool apply_box_log( Config& c, const char* v )
    {
        c.box_log = v;
        return true;
    }

    bool apply_cal( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 1 || n > 10000 )
            return value_error( "--cal", v, "1..10000" );
        c.cal_frames = n;
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

    bool apply_cal_interval( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 1 || n > 10000 )
            return value_error( "--cal-interval", v, "1..10000" );
        c.cal_interval = n;
        return true;
    }

    bool apply_conf( Config& c, const char* v )
    {
        float f = NAN;
        if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
            return value_error( "--conf", v, "0.0..1.0" );
        c.conf_thr = f;
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

    bool apply_cpu_draw( Config& c, const char* )
    {
        c.cpu_draw = true;
        return true;
    }

    bool apply_device( Config& c, const char* v )
    {
        c.device = v;
        return true;
    }

    bool apply_dump_cal( Config& c, const char* )
    {
        c.dump_cal = true;
        return true;
    }

    bool apply_frames( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 1 )
            return value_error( "--frames", v, ">= 1" );
        c.frames = n;
        return true;
    }

    bool apply_h( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 16 || n > 8192 )
            return value_error( "--h", v, "16..8192" );
        if ( n % 16 != 0 )
            std::cerr << "warning: --h " << n << " is not a multiple of 16: some UVC cameras reject it\n";
        c.cam_h = n;
        return true;
    }

    bool apply_iou( Config& c, const char* v )
    {
        float f = NAN;
        if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
            return value_error( "--iou", v, "0.0..1.0" );
        c.iou_thr = f;
        return true;
    }

    bool apply_model( Config& c, const char* v )
    {
        c.model = v;
        return true;
    }

    bool apply_net( Config& c, const char* )
    {
        c.net = true;
        return true;
    }

    bool apply_net_bitrate( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 1000 || n > 100000 )
            return value_error( "--net-bitrate", v, "1000..100000 kbps" );
        c.net_bitrate = n;
        return true;
    }

    bool apply_net_fps( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 0 || n > 120 )
            return value_error( "--net-fps", v, "0..120" );
        c.net_fps = n;
        return true;
    }

    bool apply_net_h264( Config& c, const char* )
    {
        c.net_h264 = true;
        return true;
    }

    bool apply_net_host( Config& c, const char* v )
    {
        c.net_host = v;
        return true;
    }

    bool apply_net_port( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 1 || n > 65535 )
            return value_error( "--net-port", v, "1..65535" );
        c.net_port = n;
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

    bool apply_w( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < 16 || n > 8192 )
            return value_error( "--w", v, "16..8192" );
        if ( n % 16 != 0 )
            std::cerr << "warning: --w " << n << " is not a multiple of 16: some UVC cameras reject it\n";
        c.cam_w = n;
        return true;
    }
}  // namespace

OptionRegistry::OptionRegistry()
  : table_ { {
        { "--box-log", { true, "per-frame box CSV for jitter A/B (raw vs smoothed)", apply_box_log } },
        { "--cal", { true, "calibration frames 1..10000", apply_cal } },
        { "--cal-dir", { true, "directory for the --cal series (default: dataset/)", apply_cal_dir } },
        { "--cal-interval", { true, "calibration sampling stride 1..10000", apply_cal_interval } },
        { "--conf", { true, "confidence threshold 0.0..1.0", apply_conf } },
        { "--config", { true, "INI config file (key = value); applied BEFORE the CLI options", apply_config } },
        { "--core", { true, "NPU cores: 0 auto, 1/2/4 = core 0/1/2, 3 (0+1), 7 (0+1+2); ignored with --pool > 1", apply_core } },
        { "--cpu-draw", { false, "draw boxes on CPU (A/B vs GPU drawer)", apply_cpu_draw } },
        { "--device", { true, "camera device path", apply_device } },
        { "--dump-cal", { false, "collect calibration dataset", apply_dump_cal } },
        { "--frames", { true, "stop after N frames (>= 1; default: run until Ctrl-C)", apply_frames } },
        { "--h", { true, "camera height", apply_h } },
        { "--iou", { true, "NMS IoU threshold 0.0..1.0", apply_iou } },
        { "--model", { true, "RKNN model path", apply_model } },
        { "--net", { false, "network preview (raw rgb24 or --net-h264)", apply_net } },
        { "--net-bitrate", { true, "preview bitrate 1000..100000 kbps", apply_net_bitrate } },
        { "--net-fps", { true, "preview stream fps 1..120, 0 = follow the source (VFR pts from dequeue times)", apply_net_fps } },
        { "--net-h264", { false, "H.264/MPEG-TS preview instead of raw rgb24", apply_net_h264 } },
        { "--net-host", { true, "optional UDP destination (h264 push); unset = TCP pull", apply_net_host } },
        { "--net-port", { true, "preview port 1..65535", apply_net_port } },
        { "--net-src", { true, "network MJPEG pull source (http://host:port/path); replaces --device", apply_net_src } },
        { "--pool", { true, "NPU context pool: 1 = single context (baseline), 3 = one engine per core", apply_pool } },
        { "--w", { true, "camera width", apply_w } },
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
