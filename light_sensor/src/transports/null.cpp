#include <cstddef>
#include "transports/null.h"

bool NullTransport::init() {
    // Для заглушки инициализация всегда успешна.
    // Если хочешь видеть логи при старте (даже в тесте) — раскомментируй:
    // #ifdef PLATFORM_TEST
    //     // Используй printf или свой логгер, если нет iostream
    //     printf("[NULL] Transport initialized (test mode)\n");
    // #endif
    return true;
}

bool NullTransport::send(const unsigned char* data, std::size_t len) {
    (void)data;
    (void)len;
#ifdef DEBUG_NULL_SEND
    // printf("[NULL] send(%zu bytes)\n", len);
#endif
    return true;
}
