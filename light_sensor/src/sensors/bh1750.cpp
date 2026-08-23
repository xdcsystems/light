#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.hpp"
#include "sensors/bh1750.h"

namespace {
constexpr const char* kTag = "Bh1750";
constexpr uint8_t kPowerOn = 0x01;
constexpr uint8_t kReset = 0x07;
constexpr uint8_t kContHRes = 0x10;  // 1 lx, ~120 ms
constexpr int kCmdTimeoutMs = 100;
constexpr int kFirstSampleMs = 180;
}  // namespace

bool Bh1750::write_cmd(uint8_t opcode) {
    return i2c_master_transmit(dev_, &opcode, 1, pdMS_TO_TICKS(kCmdTimeoutMs)) == ESP_OK;
}

bool Bh1750::init(uint8_t address) {
    if (ready_) {
        return true;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(I2C_SDA_GPIO);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(I2C_SCL_GPIO);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_master_probe(bus_, address, pdMS_TO_TICKS(kCmdTimeoutMs));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "BH1750 not found at 0x%02X (SDA=%d SCL=%d): %s",
                 static_cast<unsigned>(address), I2C_SDA_GPIO, I2C_SCL_GPIO,
                 esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = address;
    dev_cfg.scl_speed_hz = I2C_HZ;

    err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!write_cmd(kPowerOn) || !write_cmd(kReset) || !write_cmd(kContHRes)) {
        ESP_LOGE(kTag, "BH1750 mode setup failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(kFirstSampleMs));
    ready_ = true;
    ESP_LOGI(kTag, "BH1750FVI ready at 0x%02X", static_cast<unsigned>(address));
    return true;
}

int Bh1750::readLux() {
    if (!ready_) {
        return -1;
    }

    uint8_t raw[2] = {0, 0};
    const esp_err_t err = i2c_master_receive(dev_, raw, sizeof(raw), pdMS_TO_TICKS(kCmdTimeoutMs));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "read failed: %s", esp_err_to_name(err));
        return -1;
    }

    const uint32_t raw16 = (static_cast<uint32_t>(raw[0]) << 8) | raw[1];
    // H-resolution: lux = raw / 1.2 = raw * 5 / 6

    ESP_LOGI(kTag, "device_id=%u lux=%u", static_cast<unsigned>(DEVICE_ID), static_cast<unsigned>(raw16));

    return static_cast<int>((raw16 * 5U) / 6U);
}
