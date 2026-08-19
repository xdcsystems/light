#define _POSIX_C_SOURCE 200809L

#include "scene_scheduler.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>

namespace {

void copy_name(char* dest, size_t dest_len, const char* src) {
    if (!dest || dest_len == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }

    size_t i = 0;
    for (; i + 1 < dest_len && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

Scene make_scene(const char* name, uint16_t from_min, uint16_t to_min,
                 bool has_min, uint8_t min_brightness,
                 bool has_max, uint8_t max_brightness) {
    Scene scene;
    copy_name(scene.name, sizeof(scene.name), name);
    scene.from_min = from_min;
    scene.to_min = to_min;
    scene.has_min = has_min;
    scene.min_brightness = min_brightness;
    scene.has_max = has_max;
    scene.max_brightness = max_brightness;
    return scene;
}

uint8_t clamp_brightness(uint8_t value, const Scene& scene) {
    uint8_t lo = 0;
    uint8_t hi = 100;
    bool has_lo = scene.has_min;
    bool has_hi = scene.has_max;

    if (has_lo) {
        lo = scene.min_brightness;
    }
    if (has_hi) {
        hi = scene.max_brightness;
    }
    if (has_lo && has_hi && lo > hi) {
        const uint8_t tmp = lo;
        lo = hi;
        hi = tmp;
    }

    if (has_lo && value < lo) {
        value = lo;
    }
    if (has_hi && value > hi) {
        value = hi;
    }
    return value;
}

} // namespace

bool parse_hhmm(const char* text, uint16_t& minutes) {
    if (!text || text[0] == '\0') {
        return false;
    }

    unsigned hour = 0;
    unsigned minute = 0;
    char extra = '\0';
    const int n = std::sscanf(text, "%u:%u%c", &hour, &minute, &extra);
    if (n != 2 || hour > 23 || minute > 59) {
        return false;
    }

    minutes = static_cast<uint16_t>(hour * 60u + minute);
    return true;
}

bool format_hhmm(uint16_t minutes, char* out, size_t out_len) {
    if (!out || out_len < 6 || minutes > 1439) {
        return false;
    }

    const unsigned hour = minutes / 60;
    const unsigned minute = minutes % 60;
    std::snprintf(out, out_len, "%02u:%02u", hour, minute);
    return true;
}

bool minutes_in_range(uint16_t now, uint16_t from, uint16_t to) {
    if (from == to) {
        return true;
    }
    if (from < to) {
        return now >= from && now < to;
    }
    return now >= from || now < to;
}

uint16_t local_minutes_now() {
    const time_t now = time(nullptr);
    struct tm local{};
    if (localtime_r(&now, &local) == nullptr) {
        return 0;
    }
    return static_cast<uint16_t>(local.tm_hour * 60 + local.tm_min);
}

void SceneScheduler::reset_to_defaults() {
    clear();
    add(make_scene("morning", 6 * 60, 11 * 60, false, 0, false, 100));
    add(make_scene("day", 11 * 60, 18 * 60, false, 0, false, 100));
    add(make_scene("night", 18 * 60, 6 * 60, true, 10, true, 40));
}

void SceneScheduler::clear() {
    count_ = 0;
}

bool SceneScheduler::add(const Scene& scene) {
    if (count_ >= kMaxScenes) {
        return false;
    }
    scenes_[count_] = scene;
    if (scenes_[count_].name[0] == '\0') {
        copy_name(scenes_[count_].name, sizeof(scenes_[count_].name), "scene");
    }
    ++count_;
    return true;
}

const Scene* SceneScheduler::select(uint16_t minutes_since_midnight) const {
    for (size_t i = 0; i < count_; ++i) {
        if (minutes_in_range(minutes_since_midnight,
                             scenes_[i].from_min,
                             scenes_[i].to_min)) {
            return &scenes_[i];
        }
    }
    return nullptr;
}

BrightnessPolicy::BrightnessPolicy() {
    reset_to_defaults();
}

void BrightnessPolicy::reset_to_defaults() {
    default_curve_.reset_to_defaults();
    clear_named();
    scheduler_.reset_to_defaults();
}

void BrightnessPolicy::set_default_curve(const BrightnessCurve& curve) {
    default_curve_ = curve;
}

void BrightnessPolicy::clear_named() {
    named_count_ = 0;
}

bool BrightnessPolicy::add_named_entry(const char* set_name,
                                       uint16_t lux_below,
                                       uint8_t brightness) {
    if (!set_name || set_name[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < named_count_; ++i) {
        if (std::strcmp(named_[i].name, set_name) == 0) {
            return named_[i].curve.add(lux_below, brightness);
        }
    }

    if (named_count_ >= kMaxNamed) {
        return false;
    }

    copy_name(named_[named_count_].name, sizeof(named_[named_count_].name), set_name);
    named_[named_count_].curve.clear();
    if (!named_[named_count_].curve.add(lux_below, brightness)) {
        return false;
    }
    ++named_count_;
    return true;
}

void BrightnessPolicy::sort_named() {
    for (size_t i = 0; i < named_count_; ++i) {
        named_[i].curve.sort();
    }
}

const BrightnessCurve* BrightnessPolicy::resolve_curve(const char* map_set) const {
    if (!map_set || map_set[0] == '\0') {
        return &default_curve_;
    }

    for (size_t i = 0; i < named_count_; ++i) {
        if (std::strcmp(named_[i].name, map_set) == 0) {
            return &named_[i].curve;
        }
    }

    return &default_curve_;
}

uint8_t BrightnessPolicy::apply(uint16_t lux, char* scene_name, size_t scene_name_len) const {
    return apply_at(lux, local_minutes_now(), scene_name, scene_name_len);
}

uint8_t BrightnessPolicy::apply_at(uint16_t lux, uint16_t minutes,
                                   char* scene_name, size_t scene_name_len) const {
    const Scene* scene = scheduler_.select(minutes);
    if (scene_name && scene_name_len > 0) {
        scene_name[0] = '\0';
        if (scene) {
            copy_name(scene_name, scene_name_len, scene->name);
        }
    }

    const BrightnessCurve* curve = &default_curve_;
    if (scene) {
        curve = resolve_curve(scene->map_set);
    }

    uint8_t brightness = curve->apply(lux);
    if (scene) {
        brightness = clamp_brightness(brightness, *scene);
    }
    return brightness;
}
