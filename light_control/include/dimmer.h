#pragma once

#include <cstdint>

class Logger;

// Software dimmer output. The stub logs the value that would go to hardware.
class Dimmer {
public:
    explicit Dimmer(Logger& logger);

    void set_brightness(uint8_t brightness);

    uint8_t last_brightness() const { return last_; }
    bool has_output() const { return has_last_; }

private:
    Logger& logger_;
    uint8_t last_ = 0;
    bool has_last_ = false;
};
