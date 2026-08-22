#pragma once

#include <cstddef>
#include <cstdint>

#include "brightness_curve.h"

// HH:MM → minutes from midnight (0–1439). Rejects extra characters.
bool parse_hhmm(const char* text, uint16_t& minutes);

// Minutes 0–1439 → "HH:MM". Needs at least 6 bytes in `out`.
bool format_hhmm(uint16_t minutes, char* out, size_t out_len);

// Inclusive start, exclusive end. from == to means the whole day.
// from > to wraps past midnight (night 18:00–06:00).
bool minutes_in_range(uint16_t now, uint16_t from, uint16_t to);

uint16_t local_minutes_now();

struct Scene {
    char name[16] = "";
    uint16_t from_min = 0;
    uint16_t to_min = 0;
    char map_set[16] = "";
    uint8_t min_brightness = 0;
    uint8_t max_brightness = 100;
    bool has_min = false;
    bool has_max = false;
};

class SceneScheduler {
public:
    static constexpr size_t kMaxScenes = 8;

    void reset_to_defaults();
    void clear();
    bool add(const Scene& scene);

    const Scene* select(uint16_t minutes_since_midnight) const;

    size_t size() const { return count_; }
    const Scene& at(size_t index) const { return scenes_[index]; }

private:
    Scene scenes_[kMaxScenes]{};
    size_t count_ = 0;
};

// Default lux map + optional named map sets + time-of-day scenes.
class BrightnessPolicy {
public:
    static constexpr size_t kMaxNamed = 4;

    struct NamedCurve {
        char name[16] = "";
        BrightnessCurve curve;
    };

    BrightnessPolicy();

    void reset_to_defaults();

    void set_default_curve(const BrightnessCurve& curve);
    BrightnessCurve& default_curve() { return default_curve_; }
    const BrightnessCurve& default_curve() const { return default_curve_; }

    void clear_named();
    bool add_named_entry(const char* set_name, uint16_t lux_below, uint8_t brightness);
    void sort_named();

    size_t named_count() const { return named_count_; }
    const NamedCurve& named_at(size_t index) const { return named_[index]; }

    SceneScheduler& scheduler() { return scheduler_; }
    const SceneScheduler& scheduler() const { return scheduler_; }

    uint8_t apply(uint16_t lux, char* scene_name, size_t scene_name_len) const;
    uint8_t apply_at(uint16_t lux, uint16_t minutes,
                     char* scene_name, size_t scene_name_len) const;

private:
    const BrightnessCurve* resolve_curve(const char* map_set) const;

    BrightnessCurve default_curve_;
    NamedCurve named_[kMaxNamed]{};
    size_t named_count_ = 0;
    SceneScheduler scheduler_;
};
