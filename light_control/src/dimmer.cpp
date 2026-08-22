#include "dimmer.h"

#include "logger.h"

Dimmer::Dimmer(Logger& logger)
    : logger_(logger) {}

void Dimmer::set_brightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }

    if (has_last_ && last_ == brightness) {
        return;
    }

    last_ = brightness;
    has_last_ = true;
    logger_.info("dimmer set_brightness %u", static_cast<unsigned>(brightness));
}
