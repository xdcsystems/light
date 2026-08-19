#pragma once

#include <cstdint>

// Manual dimmer hold. While active, sensor-computed brightness is ignored.
class BrightnessOverride {
public:
    void set(uint8_t brightness) {
        if (brightness > 100) {
            brightness = 100;
        }
        value_ = brightness;
        active_ = true;
    }

    void clear() {
        active_ = false;
    }

    bool active() const { return active_; }
    uint8_t value() const { return value_; }

    uint8_t apply(uint8_t computed) const {
        return active_ ? value_ : computed;
    }

private:
    bool active_ = false;
    uint8_t value_ = 0;
};
