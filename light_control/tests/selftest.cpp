#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "brightness_curve.h"
#include "brightness_override.h"
#include "protocol_parser.h"
#include "scene_scheduler.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

bool scene_is(const Scene* scene, const char* name) {
    return scene != nullptr && std::strcmp(scene->name, name) == 0;
}

} // namespace

int main() {
    char two[2] = {1, 60};
    auto too_short = ProtocolParser::parse(two, 2);
    expect(!too_short.is_valid, "reject 2-byte packet");

    char four[4] = {1, 0, 50, 0};
    auto too_long = ProtocolParser::parse(four, 4);
    expect(!too_long.is_valid, "reject 4-byte packet");

    unsigned char pkt[3] = {1, 0x00, 0xDC};
    auto evening = ProtocolParser::parse(reinterpret_cast<const char*>(pkt), 3);
    expect(evening.is_valid, "accept 3-byte packet");
    expect(evening.device_id == 1, "device_id");
    expect(evening.lux == 220, "lux 220");

    unsigned char hi[3] = {7, 0x12, 0x34};
    auto high = ProtocolParser::parse(reinterpret_cast<const char*>(hi), 3);
    expect(high.is_valid && high.device_id == 7 && high.lux == 0x1234,
           "lux 0x1234 big-endian");

    BrightnessCurve curve;
    expect(curve.apply(50) == 100, "lux 50 -> 100");
    expect(curve.apply(199) == 100, "lux 199 -> 100");
    expect(curve.apply(200) == 60, "lux 200 -> 60");
    expect(curve.apply(220) == 60, "lux 220 -> 60");
    expect(curve.apply(400) == 30, "lux 400 -> 30");
    expect(curve.apply(10000) == 30, "lux 10000 -> 30");

    uint16_t minutes = 0;
    expect(parse_hhmm("06:00", minutes) && minutes == 6 * 60, "parse 06:00");
    expect(parse_hhmm("18:00", minutes) && minutes == 18 * 60, "parse 18:00");
    expect(parse_hhmm("6:05", minutes) && minutes == 6 * 60 + 5, "parse 6:05");
    expect(!parse_hhmm("24:00", minutes), "reject 24:00");
    expect(!parse_hhmm("06:60", minutes), "reject 06:60");
    expect(!parse_hhmm("06:00x", minutes), "reject extra chars");

    char hhmm[8] = {0};
    expect(format_hhmm(6 * 60, hhmm, sizeof(hhmm)) && std::strcmp(hhmm, "06:00") == 0,
           "format 06:00");
    expect(format_hhmm(18 * 60, hhmm, sizeof(hhmm)) && std::strcmp(hhmm, "18:00") == 0,
           "format 18:00");
    expect(!format_hhmm(1440, hhmm, sizeof(hhmm)), "reject format 1440");
    expect(!format_hhmm(0, hhmm, 5), "reject short format buffer");

    expect(minutes_in_range(6 * 60, 6 * 60, 11 * 60), "morning start inclusive");
    expect(!minutes_in_range(11 * 60, 6 * 60, 11 * 60), "morning end exclusive");
    expect(minutes_in_range(22 * 60, 18 * 60, 6 * 60), "night evening wrap");
    expect(minutes_in_range(0, 18 * 60, 6 * 60), "night midnight wrap");
    expect(minutes_in_range(5 * 60 + 59, 18 * 60, 6 * 60), "night before morning");
    expect(!minutes_in_range(6 * 60, 18 * 60, 6 * 60), "night ends at 06:00");

    SceneScheduler scheduler;
    scheduler.reset_to_defaults();
    expect(scene_is(scheduler.select(6 * 60), "morning"), "06:00 morning");
    expect(scene_is(scheduler.select(10 * 60 + 59), "morning"), "10:59 morning");
    expect(scene_is(scheduler.select(12 * 60), "day"), "12:00 day");
    expect(scene_is(scheduler.select(18 * 60), "night"), "18:00 night");
    expect(scene_is(scheduler.select(22 * 60), "night"), "22:00 night");
    expect(scene_is(scheduler.select(0), "night"), "00:00 night");

    BrightnessPolicy policy;
    char scene_name[16] = {0};

    uint8_t b = policy.apply_at(50, 8 * 60, scene_name, sizeof(scene_name));
    expect(b == 100 && std::strcmp(scene_name, "morning") == 0,
           "50 lux at 08:00 -> 100 morning");

    b = policy.apply_at(220, 12 * 60, scene_name, sizeof(scene_name));
    expect(b == 60 && std::strcmp(scene_name, "day") == 0,
           "220 lux at 12:00 -> 60 day");

    b = policy.apply_at(220, 22 * 60, scene_name, sizeof(scene_name));
    expect(b == 40 && std::strcmp(scene_name, "night") == 0,
           "220 lux at 22:00 clamped to night max 40");

    b = policy.apply_at(50, 22 * 60, scene_name, sizeof(scene_name));
    expect(b == 40, "50 lux at 22:00 curve 100 clamped to 40");

    b = policy.apply_at(10000, 22 * 60, scene_name, sizeof(scene_name));
    expect(b == 30, "10000 lux at 22:00 stays 30 above night min");

    BrightnessPolicy named;
    named.scheduler().clear();
    Scene morning;
    std::strncpy(morning.name, "morning", sizeof(morning.name) - 1);
    morning.from_min = 6 * 60;
    morning.to_min = 11 * 60;
    std::strncpy(morning.map_set, "daylight", sizeof(morning.map_set) - 1);
    named.scheduler().add(morning);

    expect(named.add_named_entry("daylight", 300, 80), "add named map entry");
    expect(named.add_named_entry("daylight", 65535, 20), "add named map fallback");
    named.sort_named();

    b = named.apply_at(50, 8 * 60, scene_name, sizeof(scene_name));
    expect(b == 80 && std::strcmp(scene_name, "morning") == 0,
           "named daylight map 50 lux -> 80");
    b = named.apply_at(400, 8 * 60, scene_name, sizeof(scene_name));
    expect(b == 20, "named daylight map 400 lux -> 20");

    BrightnessOverride ov;
    expect(!ov.active() && ov.apply(60) == 60, "override inactive passes through");
    ov.set(20);
    expect(ov.active() && ov.value() == 20 && ov.apply(60) == 20, "override holds 20");
    ov.set(0);
    expect(ov.active() && ov.apply(100) == 0, "override 0 is off");
    ov.set(150);
    expect(ov.value() == 100 && ov.apply(30) == 100, "override clamps 150 to 100");
    ov.clear();
    expect(!ov.active() && ov.apply(60) == 60, "override released");

    BrightnessPolicy reloaded;
    BrightnessCurve dim;
    dim.clear();
    expect(dim.add(65535, 5), "reload curve 65535 -> 5");
    reloaded.set_default_curve(dim);
    ov.set(40);
    b = reloaded.apply_at(50, 8 * 60, scene_name, sizeof(scene_name));
    expect(ov.apply(b) == 40, "reload keeps override while held");
    ov.clear();
    b = reloaded.apply_at(50, 8 * 60, scene_name, sizeof(scene_name));
    expect(b == 5, "reload curve applies after release");

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::printf("all checks passed\n");
    return 0;
}
