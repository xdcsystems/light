#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

class UdpPosixTransport {
public:
    UdpPosixTransport() = default;
    explicit UdpPosixTransport(const char* ip, uint16_t port);
    ~UdpPosixTransport();

    bool init();
    bool send(const unsigned char* data, std::size_t len);

private:
    struct sockaddr_in _addr{};

    int _fd = -1;
    const char* _ip = nullptr;
    uint16_t _port = 0;
};
