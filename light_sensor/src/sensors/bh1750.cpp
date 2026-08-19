#include "sensors/bh1750.h"

#include "config.hpp"

// I2C чипа — 2 байта raw. UDP наружу — 3 байта [id][lux_hi][lux_lo].
// Это разные протоколы; длина UDP-пакета не конфликтует с шиной датчика.
//
// BH1750FVI (GY-302) на ESP32-WROOM-32 30-pin, питание только 3.3 V:
//   VCC -> 3V3
//   GND -> GND
//   SDA -> GPIO21 (I2C_SDA_GPIO)
//   SCL -> GPIO22 (I2C_SCL_GPIO)
//   ADDR -> GND = 0x23, VCC = 0x5C

#if defined(PLATFORM_ESP32)

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "Bh1750";

static constexpr uint8_t kPowerOn = 0x01;
static constexpr uint8_t kReset = 0x07;
static constexpr uint8_t kContHRes = 0x10;  // 1 lx, ~120 ms
static constexpr int kCmdTimeoutMs = 100;
static constexpr int kFirstSampleMs = 180;

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_ready = false;

static bool write_cmd(uint8_t opcode) {
    return i2c_master_transmit(s_dev, &opcode, 1, pdMS_TO_TICKS(kCmdTimeoutMs)) == ESP_OK;
}

bool Bh1750::init(uint8_t address) {
    if (s_ready) {
        return true;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(I2C_SDA_GPIO);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(I2C_SCL_GPIO);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_master_probe(s_bus, address, pdMS_TO_TICKS(kCmdTimeoutMs));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 not found at 0x%02X (SDA=%d SCL=%d): %s",
                 static_cast<unsigned>(address), I2C_SDA_GPIO, I2C_SCL_GPIO,
                 esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address;
    dev_cfg.scl_speed_hz = I2C_HZ;

    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!write_cmd(kPowerOn) || !write_cmd(kReset) || !write_cmd(kContHRes)) {
        ESP_LOGE(TAG, "BH1750 mode setup failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(kFirstSampleMs));
    s_ready = true;
    ESP_LOGI(TAG, "BH1750FVI ready at 0x%02X", static_cast<unsigned>(address));
    return true;
}

int Bh1750::readLux() {
    if (!s_ready) {
        return -1;
    }

    uint8_t raw[2] = {0, 0};
    const esp_err_t err = i2c_master_receive(s_dev, raw, sizeof(raw), pdMS_TO_TICKS(kCmdTimeoutMs));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        return -1;
    }

    const uint32_t raw16 = (static_cast<uint32_t>(raw[0]) << 8) | raw[1];
    // H-resolution: lux = raw / 1.2 = raw * 5 / 6
    return static_cast<int>((raw16 * 5U) / 6U);
}

#else

bool Bh1750::init(uint8_t address) {
    (void)address;
    return true;
}

int Bh1750::readLux() {
    return 250;
}

#endif
