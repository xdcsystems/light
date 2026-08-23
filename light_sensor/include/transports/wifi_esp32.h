#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"

class WifiEsp32Transport {
public:
    WifiEsp32Transport() = default;
    explicit WifiEsp32Transport(const char* ssid, const char* pass, const char* ip, uint16_t port)
        : _ssid(ssid), _pass(pass), _ip(ip), _port(port) {}

    ~WifiEsp32Transport();

    bool init();
    bool send(const unsigned char* data, std::size_t len);

private:
    static void event_handler(void* arg, esp_event_base_t base, int32_t id, void* data);

    const char* _ssid = nullptr;
    const char* _pass = nullptr;
    const char* _ip = nullptr;
    uint16_t _port = 0;

    int _sock = -1;
    bool _initialized = false;

    struct sockaddr_in _addr{};

    StaticEventGroup_t _events_mem{};
    EventGroupHandle_t _events = nullptr;
    int _retry = 0;
};
