// option_registry.cpp (2026-11-08): the option table, extracted from
// cli.cpp. One registry of names, ranges and error texts; both the
// argv loop (CliOptions::parse) and the INI config-file handler
// validate through it, so both sources reject identically.
// The rows are split into four group builders (make_* below): the
// single constructor literal counted 258 lines for readability-
// function-size (threshold 200); merge() relinks nodes, no copies.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
        // strtof парсит "nan"/"NAN(...)" легально и без ERANGE; NaN проходит
        // любой range-чек выше (сравнения с NaN всегда false) и выключает
        // порог в collect_head (сравнение "conf >= thr" тоже ложно для NaN).
        // Отбраковываем и inf-литералы тем же предикатом.
        if ( !std::isfinite( v ) )
            return false;
        *out = v;
        return true;
    }

    // Unified value-rejection report; expected == nullptr omits the hint
    // (--frames/--w/--h have no range and never printed one).
    bool value_error( const char* opt, const char* val, const char* expected )
    {
        std::cerr << "invalid value for " << opt << ": '" << val << '\'';
        if ( expected )
            std::cerr << " (expected " << expected << ')';
        std::cerr << '\n';
        return false;
    }

    std::map<std::string, OptionRegistry::Option> make_flag_options()
    {
        // --- flags (no value) ---
        return {

            { "--dump-cal",
                    { false,
                            "collect calibration dataset",
                            []( Config& c, const char* ) {
                                c.dump_cal = true;
                                return true;
                            } } },
            { "--net",
                    { false,
                            "network preview (raw rgb24 or --net-h264)",
                            []( Config& c, const char* ) {
                                c.net = true;
                                return true;
                            } } },
            { "--net-h264",
                    { false,
                            "H.264/MPEG-TS preview instead of raw rgb24",
                            []( Config& c, const char* ) {
                                c.net_h264 = true;
                                return true;
                            } } },
            // M3.7: force the CPU box-drawing path (A/B vs the GPU drawer).
            { "--cpu-draw",
                    { false,
                            "draw boxes on CPU (A/B vs GPU drawer)",
                            []( Config& c, const char* ) {
                                c.cpu_draw = true;
                                return true;
                            } } },
        };
    }

    std::map<std::string, OptionRegistry::Option> make_source_options()
    {
        return {

            { "--config",
                    { true,
                            "INI config file (key = value); applied BEFORE the CLI options",
                            []( Config& c, const char* ) {
                                // Loaded by the parse() pre-pass (defaults
                                // < file < CLI); this no-op apply only keeps
                                // the table the single registry of names.
                                ( void ) c;
                                return true;
                            } } },
            { "--device",
                    { true,
                            "camera device path",
                            []( Config& c, const char* v ) {
                                c.device = v;
                                return true;
                            } } },
            { "--model",
                    { true,
                            "RKNN model path",
                            []( Config& c, const char* v ) {
                                c.model = v;
                                return true;
                            } } },

            { "--conf",
                    { true,
                            "confidence threshold 0.0..1.0",
                            []( Config& c, const char* v ) {
                                float f = NAN;
                                if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
                                    return value_error( "--conf", v, "0.0..1.0" );
                                c.conf_thr = f;
                                return true;
                            } } },
            { "--iou",
                    { true,
                            "NMS IoU threshold 0.0..1.0",
                            []( Config& c, const char* v ) {
                                float f = NAN;
                                if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
                                    return value_error( "--iou", v, "0.0..1.0" );
                                c.iou_thr = f;
                                return true;
                            } } },
        };
    }

    std::map<std::string, OptionRegistry::Option> make_run_options()
    {
        return {

            { "--cal",
                    { true,
                            "calibration frames 1..10000",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 1 || n > 10000 )
                                    return value_error( "--cal", v, "1..10000" );
                                c.cal_frames = n;
                                return true;
                            } } },
            { "--cal-dir",
                    { true,
                            "directory for the --cal series (default: dataset/)",
                            []( Config& c, const char* v ) {
                                std::error_code ec;
                                std::filesystem::create_directories( v, ec );
                                if ( ec ) {
                                    std::cerr << "invalid value for --cal-dir: '"
                                              << v << "' (" << ec.message() << ")\n";
                                    return false;
                                }
                                // write probe: an existing but read-only
                                // directory must fail here, not mid-run
                                const std::string probe =
                                        std::string( v ) + "/.light_ai_write_test";
                                bool ok = false;
                                {
                                    std::ofstream f( probe );
                                    ok = f.good();
                                }
                                std::filesystem::remove( probe, ec );
                                if ( !ok ) {
                                    std::cerr << "invalid value for --cal-dir: '"
                                              << v << "' (not writable)\n";
                                    return false;
                                }
                                c.cal_dir = v;
                                return true;
                            } } },
            { "--cal-interval",
                    { true,
                            "calibration sampling stride 1..10000",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 1 || n > 10000 )
                                    return value_error( "--cal-interval", v, "1..10000" );
                                c.cal_interval = n;
                                return true;
                            } } },

            { "--frames",
                    { true,
                            "stop after N frames (>= 1; default: run until Ctrl-C)",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                // 0 used to fall through to run() with an
                                // empty loop and exit code 1, silently.
                                // Loud rejection instead (2026-11-07),
                                // same policy as --cal; the no-value
                                // default (Config::frames = 0) remains the
                                // way to run until Ctrl-C.
                                if ( !parse_int( v, &n ) || n < 1 )
                                    return value_error( "--frames", v, ">= 1" );
                                c.frames = n;
                                return true;
                            } } },
            { "--w",
                    { true,
                            "camera width",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 16 || n > 8192 )
                                    return value_error( "--w", v, "16..8192" );
                                if ( n % 16 != 0 )
                                    std::cerr << "warning: --w " << n << " is not a multiple of 16: some UVC cameras reject it\n";
                                c.cam_w = n;
                                return true;
                            } } },
            { "--h",
                    { true,
                            "camera height",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 16 || n > 8192 )
                                    return value_error( "--h", v, "16..8192" );
                                if ( n % 16 != 0 )
                                    std::cerr << "warning: --h " << n << " is not a multiple of 16: some UVC cameras reject it\n";
                                c.cam_h = n;
                                return true;
                            } } },

            { "--core",
                    { true,
                            "NPU cores: 0 auto, 1/2/4 = core 0/1/2, 3 (0+1), 7 (0+1+2); ignored with --pool > 1",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                // Exactly the masks set_core_mask() implements: anything else
                                // silently degrades to the runtime default (auto).
                                if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 2 && n != 3 && n != 4 && n != 7 ) )
                                    return value_error( "--core", v, "0|1|2|3|4|7" );
                                c.core = n;
                                return true;
                            } } },
            { "--pool",
                    { true,
                            "NPU context pool: 1 = single context (baseline), 3 = one engine per core",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 3 ) )
                                    return value_error( "--pool", v, "0|1|3" );
                                c.pool = n;
                                return true;
                            } } },
        };
    }

    std::map<std::string, OptionRegistry::Option> make_preview_options()
    {
        return {

            { "--net-port",
                    { true,
                            "preview port 1..65535",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 1 || n > 65535 )
                                    return value_error( "--net-port", v, "1..65535" );
                                c.net_port = n;
                                return true;
                            } } },
            { "--net-fps",
                    { true,
                            "preview stream fps 1..120, 0 = follow the source (VFR pts from dequeue times)",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 0 || n > 120 )
                                    return value_error( "--net-fps", v, "0..120" );
                                c.net_fps = n;
                                return true;
                            } } },
            { "--net-bitrate",
                    { true,
                            "preview bitrate 1000..100000 kbps",
                            []( Config& c, const char* v ) {
                                int n = 0;
                                if ( !parse_int( v, &n ) || n < 1000 || n > 100000 )
                                    return value_error( "--net-bitrate", v, "1000..100000 kbps" );
                                c.net_bitrate = n;
                                return true;
                            } } },
            { "--net-host",
                    { true,
                            "optional UDP destination (h264 push); unset = TCP pull",
                            []( Config& c, const char* v ) {
                                c.net_host = v;
                                return true;
                            } } },
            { "--box-log",
                    { true,
                            "per-frame box CSV for jitter A/B (raw vs smoothed)",
                            []( Config& c, const char* v ) {
                                c.box_log = v;
                                return true;
                            } } },
            { "--net-src",
                    { true,
                            "network MJPEG pull source (http://host:port/path); replaces --device",
                            []( Config& c, const char* v ) {
                                if ( std::strncmp( v, "http://", 7 ) != 0 )
                                    return value_error( "--net-src", v, "http://host:port/path" );
                                c.net_src = v;
                                c.device.clear();  // mutual exclusion by construction
                                return true;
                            } } },
        };
    }
}  // namespace

OptionRegistry::OptionRegistry()
{
    // One entry per option; flags carry needs_value = false and never touch
    // val (it arrives as nullptr). Adding an option is a single table row,
    // not another branch. std::map, not unordered_map: the table is consulted
    // ~14 times per run, lookup speed is irrelevant, while sorted iteration
    // gives --help a stable listing for free.

    // Model input dimensions are read from the model itself, so --mw/--mh are
    // intentionally gone: they could only desynchronize RGA from the real tensor.
    table_ = make_flag_options();
    table_.merge( make_source_options() );
    table_.merge( make_run_options() );
    table_.merge( make_preview_options() );
}

const OptionRegistry::Option* OptionRegistry::find( const char* name ) const
{
    const auto it = table_.find( name );
    return it == table_.end() ? nullptr : &it->second;
}

void OptionRegistry::print_usage( std::ostream& os ) const
{
    os << "usage: light_ai [options]\noptions:\n";
    for ( const auto& kv : table_ )
    {
        const Option& o = kv.second;
        os << "  " << kv.first << ( o.needs_value ? " <value>" : "" ) << " - " << o.desc << '\n';
    }
}
