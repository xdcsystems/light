#pragma once

#include <cstdint>

#include "transports/transport_wrapper.hpp"

// ID устройства — меняй здесь, если нужно для разных узлов
constexpr uint8_t DEVICE_ID = 1;

// Куда слать UDP. Перед прошивкой поставь LAN-адрес роутера с light_control.
// Для локальной петли без роутера можно временно "127.0.0.1".
constexpr const char* CONTROLLER_IP = "192.168.1.1";
constexpr uint16_t CONTROLLER_PORT = 5005;
constexpr unsigned SAMPLE_PERIOD_MS = 1000;

// BH1750FVI: ADDR к GND → 0x23, ADDR к VCC → 0x5C. GY-302 обычно 0x23.
constexpr uint8_t BH1750_I2C_ADDR = 0x23;

// UDP-пакет: ровно 3 байта [device_id][lux_hi][lux_lo] (lux uint16 big-endian).
// Это не шина BH1750: чип по I2C отдаёт те же 16 бит lux, плюс сюда добавляется device_id.

// ------------------------------------------------------------------
// ПЛАТФОРМА ЗАДАЁТСЯ ТОЛЬКО В CMake (add_compile_definitions)
// НЕ РАСКОММЕНТИРУЙ НИ ОДИН #define ЗДЕСЬ!
// ------------------------------------------------------------------

#if defined(PLATFORM_ESP32)
    #include "transports/wifi_esp32.h"
    using TransportType = TransportWrapper<WifiEsp32Transport>;
    #define TRANSPORT_HAS_WIFI

    // =====================================================================
    // ПЕРЕД ПРОШИВКОЙ ОБЯЗАТЕЛЬНО ЗАМЕНИТЬ (compile-time, потом пересборка):
    //   WIFI_SSID / WIFI_PASS  — сеть 2.4 ГГц, куда ходит роутер
    //   CONTROLLER_IP выше     — если роутер не 192.168.1.1
    // Плейсхолдеры YOUR_SSID / YOUR_PASSWORD специально ломают init():
    // в мониторе будет "Set WIFI_SSID / WIFI_PASS" и вечный delay.
    // Готовый .bin с ними лить НЕЛЬЗЯ. Открытая сеть: WIFI_PASS = "".
    // =====================================================================
    constexpr const char* WIFI_SSID = "YOUR_SSID";
    constexpr const char* WIFI_PASS = "YOUR_PASSWORD";

    // ESP32-WROOM-32 30-pin: GPIO21/22 есть на гребенке (I2C по умолчанию у Espressif).
    constexpr int I2C_SDA_GPIO = 21;
    constexpr int I2C_SCL_GPIO = 22;
    constexpr uint32_t I2C_HZ = 100000;

#elif defined(PLATFORM_LINUX)
    #include "transports/udp_posix.h"
    using TransportType = TransportWrapper<UdpPosixTransport>;
    #define TRANSPORT_HAS_IP_AND_PORT

#elif defined(PLATFORM_TEST)
    #include "transports/null.h"
    using TransportType = TransportWrapper<NullTransport>;
    // Для NullTransport не нужны параметры, поэтому макросы не задаём

#else
    #error "No platform defined! Use CMake: -DPLATFORM_TEST=1 or -DPLATFORM_LINUX=1 or -DPLATFORM_ESP32=1"
#endif
