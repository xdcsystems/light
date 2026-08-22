#pragma once

#include <cstdint>

#include "transports/transport_wrapper.hpp"

// ------------------------------------------------------------------
// ГЛОБАЛЬНЫЕ ДЕФОЛТЫ (единая точка правды для всех платформ)
// ------------------------------------------------------------------
constexpr uint8_t DEVICE_ID = 1;
constexpr unsigned SAMPLE_PERIOD_MS = 1000;

// BH1750FVI: ADDR к GND → 0x23, ADDR к VCC → 0x5C. GY-302 обычно 0x23.
constexpr uint8_t BH1750_I2C_ADDR = 0x23;

// UDP-пакет: ровно 3 байта [device_id][lux_hi][lux_lo] (lux uint16 big-endian).
// Это не шина BH1750: чип по I2C отдаёт 2 байта raw, плюс сюда добавляется device_id.

// ------------------------------------------------------------------
// ПЛАТФОРМА ЗАДАЁТСЯ ТОЛЬКО В CMake (add_compile_definitions)
// НЕ РАСКОММЕНТИРУЙ НИ ОДИН #define ЗДЕСЬ!
// ------------------------------------------------------------------

#if defined(PLATFORM_ESP32)
    #include "sdkconfig.h"
    #include "transports/wifi_esp32.h"
    using TransportType = TransportWrapper<WifiEsp32Transport>;
    #define TRANSPORT_HAS_WIFI

    // ESP-IDF подставит CONFIG_* из sdkconfig / menuconfig (Kconfig.projbuild).
    // Реальные SSID/пароль в git не кладём: default в Kconfig пустой.
    // Перед прошивкой: idf.py menuconfig → Light Sensor Configuration
    // (или правь локальный esp32/sdkconfig — он в .gitignore).
    static constexpr const char* kWifiSsid = CONFIG_LIGHT_SENSOR_WIFI_SSID;
    static constexpr const char* kWifiPass = CONFIG_LIGHT_SENSOR_WIFI_PASS;
    static constexpr const char* kControllerIp = CONFIG_LIGHT_SENSOR_CONTROLLER_IP;
    static constexpr uint16_t kControllerPort = CONFIG_LIGHT_SENSOR_CONTROLLER_PORT;

    constexpr const char* WIFI_SSID = kWifiSsid;
    constexpr const char* WIFI_PASS = kWifiPass;
    constexpr const char* CONTROLLER_IP = kControllerIp;
    constexpr uint16_t CONTROLLER_PORT = kControllerPort;

    // ESP32-WROOM-32 30-pin: GPIO21/22 есть на гребенке.
    constexpr int I2C_SDA_GPIO = 21;
    constexpr int I2C_SCL_GPIO = 22;
    constexpr uint32_t I2C_HZ = 100000;

#elif defined(PLATFORM_LINUX)
    #include "transports/udp_posix.h"
    using TransportType = TransportWrapper<UdpPosixTransport>;
    #define TRANSPORT_HAS_IP_AND_PORT

    constexpr const char* CONTROLLER_IP = "192.168.1.1";
    constexpr uint16_t CONTROLLER_PORT = 5005;

#elif defined(PLATFORM_TEST)
    #include "transports/null.h"
    using TransportType = TransportWrapper<NullTransport>;
#else
    #error "No platform defined! Use CMake: -DPLATFORM_TEST=1 or -DPLATFORM_LINUX=1 or -DPLATFORM_ESP32=1"
#endif
