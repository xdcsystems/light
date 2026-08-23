#include <cstdarg>
#include <cstdio>
#include <syslog.h>

#include "logger.h"

Logger::Logger(const char* ident) {
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_LOCAL0);
}

Logger::~Logger() {
    closelog();
}

void Logger::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(LOG_INFO, fmt, args);
    va_end(args);
}

void Logger::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(LOG_DEBUG, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(LOG_WARNING, fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(LOG_ERR, fmt, args);
    va_end(args);
}

void Logger::debug_hex(const char* prefix, const char* data, size_t len,
                       const char* ip, uint16_t port) {
    // UDP recv is 256 bytes; "XX " per byte + header fits in 1024.
    char line[1024];
    int used = std::snprintf(line, sizeof(line), "%s: len=%zu, from %s:%u, hex=",
                             prefix ? prefix : "", len, ip ? ip : "-",
                             static_cast<unsigned>(port));
    if (used < 0) {
        return;
    }

    size_t pos = static_cast<size_t>(used);
    if (pos >= sizeof(line)) {
        line[sizeof(line) - 1] = '\0';
        syslog(LOG_DEBUG, "%s", line);
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        if (pos + 4 >= sizeof(line)) {
            break;
        }
        const int n = std::snprintf(
            line + pos, sizeof(line) - pos, "%s%02X",
            i == 0 ? "" : " ",
            static_cast<unsigned>(static_cast<unsigned char>(data[i])));
        if (n < 0) {
            break;
        }
        pos += static_cast<size_t>(n);
    }
    line[sizeof(line) - 1] = '\0';
    syslog(LOG_DEBUG, "%s", line);
}
