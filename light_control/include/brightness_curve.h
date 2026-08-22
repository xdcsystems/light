#pragma once

#include <cstddef>
#include <cstdint>

// Fixed-size lux → brightness map. First matching (lux < lux_below) wins.
class BrightnessCurve {
public:
    static constexpr size_t kMaxEntries = 8;

    struct Entry {
        uint16_t lux_below = 65535;
        uint8_t brightness = 30;
    };

    BrightnessCurve();

    void reset_to_defaults();
    void clear();
    bool add(uint16_t lux_below, uint8_t brightness);
    void sort();

    uint8_t apply(uint16_t lux) const;

    size_t size() const { return count_; }
    const Entry& at(size_t index) const { return entries_[index]; }

private:
    Entry entries_[kMaxEntries]{};
    size_t count_ = 0;
};
