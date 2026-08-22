#include <cstdint>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
    #include <iostream>
#endif

#if defined(PLATFORM_ESP32)
    #include "esp_log.h"
    #include "sensors/bh1750.h"
#else
    #include "sensors/stub.h"
#endif

#include "config.hpp"
#include "platform_delay.hpp"
#include "app_core.h"

#if defined(PLATFORM_ESP32)
static const char* TAG = "light_sensor";
#endif

template <typename T>
void run_application(T& transport) {
#if defined(PLATFORM_ESP32)
    Bh1750 sensor;
#else
    Stub sensor(SensorScenario::Evening);
#endif
    if (!sensor.init(BH1750_I2C_ADDR)) {
#if defined(PLATFORM_ESP32)
        ESP_LOGE(TAG, "BH1750 init failed at 0x%02X SDA=%d SCL=%d",
                 static_cast<unsigned>(BH1750_I2C_ADDR), I2C_SDA_GPIO, I2C_SCL_GPIO);
        while (true) {
            platform_delay_ms(1000);
        }
#else
        std::cerr << "[ERROR] Sensor initialization failed\n";
        return;
#endif
    }

    uint8_t packet[3];

    while (true) {
        const int lux = sensor.readLux();
        uint16_t lux16 = 0;
        if (lux > 0) {
            lux16 = (lux > 65535) ? 65535 : static_cast<uint16_t>(lux);
        }

        packet[0] = DEVICE_ID;
        packet[1] = static_cast<uint8_t>(lux16 >> 8);
        packet[2] = static_cast<uint8_t>(lux16 & 0xFF);

#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
        std::cout << "[SENSOR] device_id=" << static_cast<int>(DEVICE_ID)
                  << " lux=" << static_cast<unsigned>(lux16) << '\n';
#elif defined(PLATFORM_ESP32)
        ESP_LOGI(TAG, "device_id=%u lux=%u",
                 static_cast<unsigned>(DEVICE_ID), static_cast<unsigned>(lux16));
#endif

        if (!transport.send(packet, sizeof(packet))) {
#if defined(PLATFORM_ESP32)
            ESP_LOGW(TAG, "UDP send failed");
#endif
        }

        platform_delay_ms(SAMPLE_PERIOD_MS);
    }
}

#if defined(PLATFORM_LINUX) || defined(TRANSPORT_HAS_IP_AND_PORT)
    #include "transports/udp_posix.h"

    template void run_application<TransportWrapper<UdpPosixTransport>>(
        TransportWrapper<UdpPosixTransport>&);

#elif defined(PLATFORM_ESP32)
    #include "transports/wifi_esp32.h"

    template void run_application<TransportWrapper<WifiEsp32Transport>>(
        TransportWrapper<WifiEsp32Transport>&);

#elif defined(PLATFORM_TEST)
    #include "transports/null.h"

    template void run_application<TransportWrapper<NullTransport>>(
        TransportWrapper<NullTransport>&);

#else
    #error "Не определена целевая платформа: PLATFORM_LINUX, PLATFORM_TEST или PLATFORM_ESP32"
#endif
