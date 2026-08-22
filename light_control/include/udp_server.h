#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <sys/types.h>

#include "brightness_override.h"
#include "dimmer.h"
#include "scene_scheduler.h"
#include "uci_settings.h"

class Logger;
class StatsCollector;
class LastStatus;

class UdpServer {
public:
    explicit UdpServer(StatsCollector& stats, LastStatus& last_status, Logger& logger);
    ~UdpServer();

    bool bind_to(const char* addr, uint16_t port);
    void apply_settings(const UciSettings& cfg);
    void set_policy(const BrightnessPolicy& policy);
    bool reload_from_uci();

    void set_brightness_override(uint8_t brightness);
    void clear_brightness_override();

    UciSettings copy_settings() const;
    bool has_override() const;
    uint8_t override_value() const;
    uint16_t bound_port() const;

    void run(std::atomic<bool>& stop_flag);
    void stop();

private:
    // Приём одного UDP‑пакета. Возвращает -1 при ошибке или если сервер остановлен.
    ssize_t recv_packet(char* buf, size_t buf_len,
                        char* ip, size_t ip_len,
                        uint16_t& port);

    void close_socket();
    void reapply_output_locked();

private:
    int sock_ = -1;                 // Один сокет, без дублей
    uint16_t bound_port_ = 0;
    StatsCollector& stats_;
    LastStatus& last_status_;
    Logger& logger_;
    Dimmer dimmer_;
    mutable std::mutex mu_;
    UciSettings cfg_;
    BrightnessOverride override_;
};
