# light_sensor

A compact, memory‑safe sensor node for ambient light measurement (BH1750) with UDP reporting. Designed for embedded platforms (ESP32, Linux) under strict memory constraints: no dynamic/static allocation, no raw pointers.

## Key Features

- **Memory discipline**: strictly avoids dynamic and static memory allocation; uses stack variables and fixed‑size buffers only.
- **Modular architecture**: sensors and transports are decoupled into separate subsystems.
- **Multiple transport backends**:
  - UDP (POSIX) for Linux/OpenWrt targets.
  - Null transport for isolated testing.
  - ESP32 Wi‑Fi transport for ESP32 platforms.
- **Test‑friendly**: dedicated test build mode with stubbed hardware (no real sensor or network required).
- **Clean build configuration**: single source file list in CMake, minimal conditional compilation in core code.

## Supported Sensors

- **BH1750FVI (GY‑302)** – primary ambient light sensor (ESP32 I2C; host builds keep a compile stub in the same file).
- **Stub sensor** – mock implementation for CI and unit‑style tests.

## Supported Transports

- **UDP (POSIX)** – sends readings over UDP on Linux systems.
- **Null transport** – no network activity; useful for logic validation and CI.
- **ESP32 Wi‑Fi** – Wi‑Fi based transport tailored for ESP32.

## Build Requirements

- CMake ≥ 3.16
- C++17 compatible compiler (e.g., GCC 12.3.0 with musl)
- POSIX environment for Linux builds

## Building the Project

### Linux (UDP POSIX transport)

    mkdir build_linux && cd build_linux
    cmake .. -DPLATFORM_LINUX=ON -DPLATFORM_TEST=OFF -DPLATFORM_ESP32=OFF
    make -j$(nproc)
    ./light_sensor

### Test Build (No Hardware Required)
Uses stub sensor and null transport to validate logic without real hardware.

    mkdir build_test && cd build_test
    cmake .. -DPLATFORM_TEST=ON -DPLATFORM_LINUX=OFF -DPLATFORM_ESP32=OFF
    make -j$(nproc)
    ./light_sensor

### ESP32 (Wi‑Fi + BH1750FVI)

Host CMake cannot cross-compile this target. Use ESP-IDF **5.2+** (`idf.py set-target esp32`) for ESP32-WROOM-32 / ESP32-D0WD-V3. IDF 6.x needs the wrapper's `-D_GNU_SOURCE` (already in `esp32/CMakeLists.txt`; harmless on 5.x).

1. Set `WIFI_SSID` and `WIFI_PASS` in [`include/config.hpp`](include/config.hpp). Placeholder values refuse to start.
2. Default I2C: **SDA GPIO21**, **SCL GPIO22**, address **0x23** (ADDR to GND). 3.3 V only.
3. Build and flash (client board talks on `/dev/ttyUSB0` via CH340C):

```sh
cd esp32
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The IDF wrapper lives in [`esp32/`](esp32/) and compiles `src/main.cpp`, `src/sensors/bh1750.cpp`, `src/transports/wifi_esp32.cpp`. UDP payload is still 3 bytes `[id][lux_hi][lux_lo]` to `CONTROLLER_IP:CONTROLLER_PORT`.

## Project Structure
    src/ – implementation files (.cpp).
    include/ – header files (.h, .hpp).
    CMakeLists.txt – build configuration.
    .gitignore – excludes build artifacts and IDE files.

### Example layout:

    light_sensor/
    ├── CMakeLists.txt          # host: LINUX / TEST only
    ├── esp32/                  # ESP-IDF project (idf.py), not host CMake
    │   ├── CMakeLists.txt
    │   ├── sdkconfig.defaults
    │   └── main/CMakeLists.txt
    ├── .gitignore
    ├── README.md
    ├── include/
    │   ├── config.hpp
    │   ├── platform_delay.hpp
    │   ├── sensors/
    │   │   ├── base.h
    │   │   ├── bh1750.h
    │   │   ├── scenario.h
    │   │   └── stub.h
    │   └── transports/
    │       ├── base.h
    │       ├── null.h
    │       ├── transport_wrapper.hpp
    │       ├── udp_posix.h
    │       └── wifi_esp32.h
    └── src/
        ├── main.cpp
        ├── sensors/
        │   ├── bh1750.cpp
        │   └── stub.cpp
        └── transports/
            ├── null.cpp
            ├── udp_posix.cpp
            └── wifi_esp32.cpp

## Configuration

Runtime behavior and platform specifics are controlled via CMake options and include/config.hpp. Avoid modifying main.cpp for platform selection; use CMake flags instead.

Platform macros injected by CMake:

- PLATFORM_LINUX – for POSIX UDP builds (host CMake).
- PLATFORM_TEST – for test builds with stubs (host CMake).
- PLATFORM_ESP32 – for ESP32 Wi‑Fi builds (set by `esp32/main/CMakeLists.txt`, not host CMake).

These macros are used in headers/sources to enable appropriate implementations without cluttering core logic.

