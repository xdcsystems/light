#include <cstdint>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
    #include <iostream>
#endif

#if defined(PLATFORM_ESP32)
    #include "esp_log.h"
#endif

#include "sensors/base.h"
#if defined(PLATFORM_ESP32)
    #include "sensors/bh1750.h"
#else
    #include "sensors/stub.h"
#endif

#include "transports/transport_wrapper.hpp"
#include "config.hpp"
#include "platform_delay.hpp"

#if defined(PLATFORM_ESP32)
static const char* TAG = "light_sensor";
#endif

#if defined(PLATFORM_ESP32)
extern "C" void app_main(void)
#else
int main()
#endif
{
#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
    std::cout << "[INIT] Light sensor node started\n";
#elif defined(PLATFORM_ESP32)
    ESP_LOGI(TAG, "node start id=%u -> %s:%u",
             static_cast<unsigned>(DEVICE_ID), CONTROLLER_IP,
             static_cast<unsigned>(CONTROLLER_PORT));
#endif

    // ВАЖНО: создаём объект сразу с нужными параметрами.
    // Никаких operator= и лишних присваиваний.
#if defined(TRANSPORT_HAS_WIFI)
    TransportType transport(WIFI_SSID, WIFI_PASS, CONTROLLER_IP, CONTROLLER_PORT);
#elif defined(TRANSPORT_HAS_IP_AND_PORT)
    TransportType transport(CONTROLLER_IP, CONTROLLER_PORT);
#else
    // Для NullTransport (и любых других без параметров) — конструктор по умолчанию
    TransportType transport;
#endif

    if (!transport.init()) {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
        std::cerr << "[ERROR] Transport initialization failed\n";
        return 1;
#else
        ESP_LOGE(TAG, "transport init failed");
        while (true) {
            platform_delay_ms(1000);
        }
#endif
    }

    // ESP32: BH1750FVI по I2C. Linux/TEST без железа: Stub Evening (220 lux).
#if defined(PLATFORM_ESP32)
    Bh1750 sensor;
#else
    Stub sensor(SensorScenario::Evening);
#endif
    if (!sensor.init(BH1750_I2C_ADDR)) {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_TEST)
        std::cerr << "[ERROR] Sensor initialization failed\n";
        return 1;
#else
        ESP_LOGE(TAG, "BH1750 init failed at 0x%02X SDA=%d SCL=%d",
                 static_cast<unsigned>(BH1750_I2C_ADDR), I2C_SDA_GPIO, I2C_SCL_GPIO);
        while (true) {
            platform_delay_ms(1000);
        }
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
