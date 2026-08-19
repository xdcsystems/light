#pragma once

#include <cstdint>
#include <mutex>

// Last successfully parsed sensor packet. Safe to read from the ubus thread.
struct LastStatusSnapshot {
    bool has_packet = false;
    uint8_t device_id = 0;
    uint16_t lux = 0;
    uint8_t brightness = 0;
    bool override = false;
    char scene[16] = "";
    char source_ip[16] = "";
    uint16_t source_port = 0;
    uint64_t unix_time = 0;
};

class LastStatus {
public:
    void update(uint8_t device_id, uint16_t lux, uint8_t brightness,
                const char* scene, const char* ip, uint16_t port,
                bool override);
    // Brightness/override only — used for a manual hold with no packet yet.
    void set_manual(uint8_t brightness, bool override);
    LastStatusSnapshot snapshot() const;

private:
    mutable std::mutex mu_;
    LastStatusSnapshot data_;
};
