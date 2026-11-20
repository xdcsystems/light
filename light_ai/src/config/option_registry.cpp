// The option table, extracted from cli.cpp.
// One registry of names, ranges and error texts; both the
// argv loop (CliOptions::parse) and the INI config-file handler
// validate through it, so both sources reject identically.
// The rejection text is built in one place, OptionRegistry::apply, from the
// row's name and `expected`.

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

    bool set_int( const char* v, int lo, int hi, int& dst )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || n < lo || n > hi )
            return false;
        dst = n;
        return true;
    }

    bool set_float_01( const char* v, float& dst )
    {
        float f = NAN;
        if ( !parse_float( v, &f ) || f < 0.0f || f > 1.0f )
            return false;
        dst = f;
        return true;
    }

    bool apply_cal_dir( Config& c, const char* v )
    {
        if ( !v || !*v )
            return false;
        c.cal.dir = v;
        return true;
    }

    bool apply_device( Config& c, const char* v )
    {
        if ( !v || !*v )
            return false;
        c.sources.push_back( { SourceKind::V4L2, v } );
        return true;
    }

    // --config is read by CliOptions::prepass before any option; its table entry only accepts the key. The
    // parameters stay unnamed: the Entry::apply signature is fixed and this one reads neither.
    bool apply_config( Config&, const char* )
    {
        return true;
    }

    bool apply_core( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 2 && n != 3 && n != 4 && n != 7 ) )
            return false;
        c.infer.core = n;
        return true;
    }

    bool apply_label_lang( Config& c, const char* v )
    {
        if ( !v )
            return false;
        const std::string_view sv( v );
        if ( sv == "en" )
        {
            c.stream.label_lang = LabelLang::En;
            return true;
        }
        if ( sv == "ru" )
        {
            c.stream.label_lang = LabelLang::Ru;
            return true;
        }
        return false;
    }

    bool apply_log_level( Config& c, const char* v )
    {
        if ( !v )
            return false;
        struct LevelMap
        {
            const char* name;
            Log::Level level;
        };
        constexpr std::array<LevelMap, 5> levels { {
            { "debug", Log::Level::Debug },
            { "info", Log::Level::Info },
            { "warn", Log::Level::Warn },
            { "error", Log::Level::Error },
            { "off", Log::Level::Off },
        } };
        const std::string_view sv( v );
        const auto* const it = std::find_if( levels.begin(), levels.end(), [sv]( const LevelMap& m ) {
            return sv == m.name;
        } );
        if ( it == levels.end() )
            return false;
        c.log_level = it->level;
        return true;
    }

    bool apply_reid_model( Config& c, const char* v )
    {
        if ( !v )
            return false;
        const std::string_view sv( v );
        if ( sv.empty() || sv == "none" || sv == "off" )
        {
            c.infer.reid_model.clear();
            return true;
        }
        c.infer.reid_model = v;
        return true;
    }

    bool parse_src_rot( const char* v, int& deg )
    {
        return set_int( v, 0, 270, deg ) && ( deg == 0 || deg == 90 || deg == 180 || deg == 270 );
    }

    bool apply_src_rot( Config& c, const char* v )
    {
        int deg = -1;
        if ( !parse_src_rot( v, deg ) )
            return false;
        if ( c.sources.empty() )
            return false;
        c.sources.back().rot_deg = deg;
        c.sources.back().rot_auto = false;
        return true;
    }

    bool apply_net_src( Config& c, const char* v )
    {
        if ( !v || std::strncmp( v, "http://", 7 ) != 0 )
            return false;
        c.sources.push_back( { SourceKind::NetMjpeg, v } );
        return true;
    }

    bool apply_pool( Config& c, const char* v )
    {
        int n = 0;
        if ( !parse_int( v, &n ) || ( n != 0 && n != 1 && n != 3 ) )
            return false;
        c.infer.pool = n;
        return true;
    }
}  // namespace

OptionRegistry::OptionRegistry( const Log& log )
  : table_ { {
        { "--box-log",      { true, "per-frame box CSV for jitter A/B (raw vs smoothed)", nullptr,
                              +[]( Config& c, const char* v ) { return set_str( v, c.box_log ); } } },
        { "--cal",          { true, "calibration frames 1..10000", "1..10000",
                              +[]( Config& c, const char* v ) { return set_int( v, 1, 10000, c.cal.frames ); } } },
        { "--cal-dir",      { true, "directory for the --cal series and --dump-cal (default: dataset/, build/)", "non-empty path",
                              &apply_cal_dir } },
        { "--cal-interval", { true, "calibration sampling stride 1..10000", "1..10000",
                              +[]( Config& c, const char* v ) { return set_int( v, 1, 10000, c.cal.interval ); } } },
        { "--conf",         { true, "confidence threshold 0.0..1.0", "0.0..1.0",
                              +[]( Config& c, const char* v ) { return set_float_01( v, c.infer.conf_thr ); } } },
        { "--config",       { true, "INI config file (key = value); applied BEFORE the CLI options", nullptr, &apply_config } },
        { "--core",         { true, "NPU cores: 0 auto, 1/2/4 = core 0/1/2, 3 (0+1), 7 (0+1+2); ignored with --pool > 1", "0|1|2|3|4|7",
                              &apply_core } },
        { "--cpu-draw",     { false, "draw boxes on CPU (A/B vs GPU drawer)", nullptr,
                              +[]( Config& c, const char* ) { return set_flag( c.stream.cpu_draw ); } } },
        { "--device",       { true, "camera device path (repeatable; default /dev/video20)", "/dev/videoN", &apply_device } },
        { "--dump-cal",     { false, "dump one model-input frame to build/cal.ppm (or <cal-dir>/cal.ppm)", nullptr,
                              +[]( Config& c, const char* ) { return set_flag( c.cal.dump_one ); } } },
        { "--frames",       { true, "stop after N frames (>= 1; default: run until Ctrl-C)", ">= 1",
                              +[]( Config& c, const char* v ) { return set_int( v, 1, INT_MAX, c.frames ); } } },
        { "--iou",          { true, "NMS IoU threshold 0.0..1.0", "0.0..1.0",
                              +[]( Config& c, const char* v ) { return set_float_01( v, c.infer.iou_thr ); } } },
        { "--label-lang",   { true, "callout label language (en|ru; default en)", "en|ru",
                              &apply_label_lang } },
        { "--label-scale",  { true, "callout text size 1..4, 11 px steps on the output tile (default 1)", "1..4",
                              +[]( Config& c, const char* v ) { return set_int( v, 1, 4, c.stream.label_scale ); } } },
        { "--log-level",    { true, "diagnostic verbosity: debug|info|warn|error|off (default info)",
                              "debug|info|warn|error|off", &apply_log_level } },
        { "--model",        { true, "RKNN model path", nullptr,
                              +[]( Config& c, const char* v ) { return set_str( v, c.infer.model ); } } },
        { "--net",          { false, "network preview (raw rgb24 or --net-h264)", nullptr,
                              +[]( Config& c, const char* ) { return set_flag( c.stream.enabled ); } } },
        { "--net-bitrate",  { true, "preview bitrate 1000..100000 kbps", "1000..100000 kbps",
                              +[]( Config& c, const char* v ) { return set_int( v, 1000, 100000, c.stream.bitrate_kbps ); } } },
        { "--net-fps",      { true, "preview stream fps 1..120, 0 = follow the source (VFR pts from publish times)", "0..120",
                              +[]( Config& c, const char* v ) { return set_int( v, 0, 120, c.stream.fps ); } } },
        { "--net-h264",     { false, "H.264/MPEG-TS preview instead of raw rgb24", nullptr,
                              +[]( Config& c, const char* ) { return set_flag( c.stream.h264 ); } } },
        { "--net-host",     { true, "optional UDP destination (h264 push); unset = TCP pull", nullptr,
                              +[]( Config& c, const char* v ) { return set_str( v, c.stream.host ); } } },
        { "--net-port",     { true, "preview port 1..65535", "1..65535",
                              +[]( Config& c, const char* v ) { return set_int( v, 1, 65535, c.stream.port ); } } },
        { "--net-src",      { true, "network MJPEG pull source (http://host:port/path; repeatable)", "http://host:port/path",
                              &apply_net_src } },
        { "--pool",         { true, "NPU context pool: 0|1 = single context (baseline), 3 = one engine per core", "0|1|3",
                              &apply_pool } },
        { "--reid-model",   { true, "Re-ID RKNN model path (default ./misc/osnet_x1_0_imagenet.rknn; 'none' disables)",
                              nullptr, &apply_reid_model } },
        { "--reid-thr",     { true, "Re-ID cosine similarity threshold 0.0..1.0 (default 0.62)", "0.0..1.0",
                              +[]( Config& c, const char* v ) { return set_float_01( v, c.infer.reid_thr ); } } },
        { "--src-rot",      { true, "override auto rotation for preceding source via RGA: 0|90|180|270 degrees",
                              "0|90|180|270", &apply_src_rot } },
    } }
  , log_( log )
{
}

const OptionRegistry::Entry* OptionRegistry::find( std::string_view name ) const
{
    // view compare, not strcmp: a string_view is not NUL-terminated
    // in general; the table names are literals, so == is exact.
    const auto *const it = std::find_if( table_.begin(), table_.end(),
                                  [name]( const Entry& e ) { return e.name == name; } );
    return it != table_.end() ? &*it : nullptr;
}

bool OptionRegistry::apply( const Entry& e, Config& cfg, const char* val ) const
{
    if ( e.opt.apply( cfg, val ) )
        return true;
    int deg = -1;
    if ( std::strcmp( e.name, "--src-rot" ) == 0 && cfg.sources.empty() && parse_src_rot( val, deg ) )
    {
        log_.error() << "--src-rot requires a preceding --device or --net-src (or --config source)\n";
        return false;
    }
    Log::Line line = log_.error();
    line << "invalid value for " << e.name << ": '" << ( val ? val : "" ) << '\'';
    if ( e.opt.expected )
        line << " (expected " << e.opt.expected << ')';
    line << '\n';
    return false;
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
