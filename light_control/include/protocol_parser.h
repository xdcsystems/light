#pragma once

#include <cstdint>
#include <cstddef>

class ProtocolParser {
public:
    struct Result {
        bool is_valid = false;
        uint8_t device_id = 0;
        uint16_t lux = 0;
        const char* error_msg = nullptr;
    };

    // UDP payload, not the BH1750 I2C frame.
    // Exactly 3 bytes: [device_id][lux_hi][lux_lo], lux as uint16 big-endian.
    // The chip still returns 16-bit lux over I2C (2 bytes); this packet adds device_id.
    static Result parse(const char* data, size_t len);
};
