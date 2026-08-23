#include "brightness_curve.h"

#include <algorithm>
#include <cstddef>

BrightnessCurve::BrightnessCurve() {
    reset_to_defaults();
}

void BrightnessCurve::reset_to_defaults() {
    clear();
    add(200, 100);
    add(400, 60);
    add(65535, 30);
    sort();
}

void BrightnessCurve::clear() {
    count_ = 0;
}

bool BrightnessCurve::add(uint16_t lux_below, uint8_t brightness) {
    if (count_ >= kMaxEntries) {
        return false;
    }
    if (lux_below == 0 || brightness > 100) {
        return false;
    }

    entries_[count_].lux_below = lux_below;
    entries_[count_].brightness = brightness;
    ++count_;
    return true;
}

void BrightnessCurve::sort() {
    std::sort(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count_),
              [](const Entry& a, const Entry& b) { return a.lux_below < b.lux_below; });
}

uint8_t BrightnessCurve::apply(uint16_t lux) const {
    for (size_t i = 0; i < count_; ++i) {
        if (lux < entries_[i].lux_below) {
            return entries_[i].brightness;
        }
    }

    if (count_ > 0) {
        return entries_[count_ - 1].brightness;
    }

    return 30;
}
