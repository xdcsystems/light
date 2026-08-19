#include "last_status.h"

#include <cstring>
#include <ctime>

void LastStatus::update(uint8_t device_id, uint16_t lux, uint8_t brightness,
                        const char* scene, const char* ip, uint16_t port,
                        bool override) {
    std::lock_guard<std::mutex> lock(mu_);
    data_.has_packet = true;
    data_.device_id = device_id;
    data_.lux = lux;
    data_.brightness = brightness;
    data_.override = override;
    data_.source_port = port;
    data_.unix_time = static_cast<uint64_t>(::time(nullptr));

    if (scene && scene[0] != '\0') {
        std::strncpy(data_.scene, scene, sizeof(data_.scene) - 1);
        data_.scene[sizeof(data_.scene) - 1] = '\0';
    } else {
        data_.scene[0] = '\0';
    }

    if (ip && ip[0] != '\0') {
        std::strncpy(data_.source_ip, ip, sizeof(data_.source_ip) - 1);
        data_.source_ip[sizeof(data_.source_ip) - 1] = '\0';
    } else {
        data_.source_ip[0] = '\0';
    }
}

void LastStatus::set_manual(uint8_t brightness, bool override) {
    std::lock_guard<std::mutex> lock(mu_);
    if (brightness > 100) {
        brightness = 100;
    }
    data_.brightness = brightness;
    data_.override = override;
}

LastStatusSnapshot LastStatus::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_;
}
