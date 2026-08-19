#pragma once

#include "base.h"

#include <cstdint>
#include <cstddef>

#include "lwip/sockets.h"

class WifiEsp32Transport : public Transport {
private:
    const char* _ssid = nullptr;
    const char* _pass = nullptr;
    const char* _ip = nullptr;
    uint16_t _port = 0;

    int _sock = -1;
    bool _initialized = false;

    struct sockaddr_in _addr{};

public:
    WifiEsp32Transport() = default;

    explicit WifiEsp32Transport(const char* ssid, const char* pass, const char* ip, uint16_t port)
        : _ssid(ssid), _pass(pass), _ip(ip), _port(port) {}

    ~WifiEsp32Transport() override;

    bool init() override;
    bool send(const unsigned char* data, std::size_t len) override;
};
