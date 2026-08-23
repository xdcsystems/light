#pragma once

#include <cstddef>
#include <cstdint>

class Logger {
public:
    explicit Logger(const char* ident);
    ~Logger();

    void info(const char* fmt, ...);
    void debug(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);

    void debug_hex(const char* prefix, const char* data, size_t len,
                   const char* ip, uint16_t port);
};