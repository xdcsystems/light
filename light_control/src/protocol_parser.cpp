#include "protocol_parser.h"

ProtocolParser::Result ProtocolParser::parse(const char* data, size_t len) {
    Result res;

    if (!data || len != 3) {
        res.error_msg = "Invalid length";
        return res;
    }

    res.device_id = static_cast<uint8_t>(data[0]);
    res.lux = static_cast<uint16_t>(
        (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8) |
        static_cast<uint16_t>(static_cast<uint8_t>(data[2])));
    res.is_valid = true;
    return res;
}
