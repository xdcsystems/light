#pragma once

#include <cstdint>

#include "scene_scheduler.h"

struct UciSettings {
    bool enabled = true;
    uint16_t port = 5005;
    char iface[32] = "lan";
    BrightnessPolicy policy;
};

// Fills `out` with defaults, then overlays /etc/config/light_control via libuci
// when built for OpenWrt. Returns true if the UCI package was loaded.
bool load_uci_settings(UciSettings& out);
