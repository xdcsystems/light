#pragma once

#include "base.h"
#include "driver/i2c_master.h"

class Bh1750 : public LightSensor {
public:
    bool init(uint8_t address) override;
    int readLux() override;

private:
    bool write_cmd(uint8_t opcode);

    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    bool ready_ = false;
};
