#include <cstdint>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "transports/udp_posix.h"


UdpPosixTransport::UdpPosixTransport(const char* ip, uint16_t port)
    : _ip(ip), _port(port) {}

UdpPosixTransport::~UdpPosixTransport() {
    if (_fd != -1) {
        close(_fd);
        _fd = -1;
    }
}

bool UdpPosixTransport::init() {
    // Если сокет уже создан — не делаем ничего (защита от повторной инициализации)
    if (_fd != -1) return true;

    // ЛОГИКА ПО УМОЛЧАНИЮ: если IP/порт не заданы в конструкторе, ставим дефолтные
    const char* use_ip = (_ip && _ip[0] != '\0') ? _ip : "192.168.1.1";
    uint16_t use_port = (_port != 0) ? _port : 5005;

    _fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_fd == -1) return false;

    _addr.sin_family = AF_INET;
    _addr.sin_port = htons(use_port);

    if (inet_pton(AF_INET, use_ip, &_addr.sin_addr) <= 0) {
        close(_fd);
        _fd = -1;
        return false;
    }

    return true;
}

bool UdpPosixTransport::send(const unsigned char* data, std::size_t len) {
    // Ленивая инициализация: если забыли вызвать init() вручную — сделаем это сами
    if (_fd == -1 && !init()) {
        return false;
    }

    ssize_t sent = sendto(_fd, data, len, 0,
                          reinterpret_cast<struct sockaddr*>(&_addr), sizeof(_addr));

    return (sent == static_cast<ssize_t>(len));
}
