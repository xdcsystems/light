# light_control

A network node / server with AI capabilities on OpenWrt. Provides lighting control, UBus API, and UDP reception.  
Works on TP‑Link TL‑WDR4300 v1 (ath79/generic), OpenWrt 23.05.5 r24106‑10cc5fcd00.

## Features

- UBus object `light_control` with the `get_stats` method (returns JSON).
- UDP server on port `5005` (`0.0.0.0:5005`) with non‑blocking reception.
- Graceful shutdown without `SIGKILL`, achieved via:
  - an atomic `stop_flag` in the thread;
  - a loop `while (!stop_flag) { uloop_run_timeout(200); }`;
  - timely call to `uloop_done()`.
- Built with CMake (ninja) + OpenWrt SDK (package mode).
- No dynamic memory allocation, pointers, or static memory allocation in the project’s core logic.
- Minimal macros: platform‑dependent logic is isolated in separate modules.
- Tuya local control (v3.3 protocol) for dimmer devices:
  - AES‑128‑ECB encryption (no IV, PKCS7 padding);
  - Correct CRC32 calculation over full packet (including prefix);
  - Version header `"3.3"` sent in clear text before encrypted payload;
  - DP mapping: `DP_SWITCH=20` (power), `DP_MODE=21` (mode), `DP_BRIGHT=22` (brightness);
  - Response parsing with retcode validation (`retcode=0` = success).
- Heartbeat mechanism to keep TCP connection alive (periodic status request).

## Dependencies

- OpenWrt SDK for `ath79/generic`.
- Compiler: GNU 12.3.0 + musl.
- OpenWrt libraries: `libubus`, `libuloop`.
- Cryptography: `libssl`, `libcrypto` (OpenSSL 1.1+), `libz` (zlib).
- Compiler: GNU 12.3.0 + musl.

>Note: In OpenWrt make menuconfig, ensure the following packages are enabled:

    - Libopenssl / Libcrypto
    - Zlib

    Without these, the build will fail at link time or the binary will fail to load on the router.



## Repository structure

    light_control/
    ├── CMakeLists.txt # Project build (CMake/ninja), single source file list
    ├── src/ # C++17 source code: UbusExporter, UdpReceiver, etc.
    ├── include/ # Header files
    ├── externals/openwrt # OpenWrt build tree (SDK)
    ├── openwrt_pkg/ # OpenWrt package (Makefile, init.d, uci-defaults)
    ├── .gitignore # Ignored files (build_dir, bin, .vscode, etc.)
    └── README.md # This documentation


>> **Note:** The real OpenWrt SDK should be cloned into `externals/openwrt` locally. This directory is ignored by Git and must not be pushed.

---

## Getting the OpenWrt SDK

Clone the SDK into the `externals/` directory:

    cd light_control/externals
    git clone https://git.openwrt.org/openwrt/openwrt.git openwrt
    cd openwrt
    git checkout openwrt-23.05   
    ./scripts/feeds update -a
    ./scripts/feeds install -a
    # Ensure libopenssl, libz are available in the feeds

## Environment setup

1. Clone the repository:
   ```sh
   git clone <url> light_control
   cd light_control

2. Place the OpenWrt SDK in externals/openwrt (or clone the build tree there).
   Make sure it’s the SDK for ath79/generic under OpenWrt 23.05.

3. Configure the SDK:
    
        cd externals/openwrt
        make defconfig
        # In make menuconfig, enable: libubus, libuloop (if not enabled by default)

4. Verify that .config contains:

        CONFIG_TARGET_ath79=y
        CONFIG_TARGET_ath79_generic=y

## Building the package

From the project root (light_control/):

    cd externals/openwrt
    make package/light_control V=s

Output: 
            
    bin/targets/ath79/generic/light_control_*.ipk.

> Do not use cmake.mk and do not copy sources into the OpenWrt build tree — the build uses a separate CMake layer plus the package Makefile.

> The package build (make package/light_control) requires that libopenssl, libz, and their dev-headers are available in the SDK. If they are missing, the linker will fail.

## Installation on the router

   On the router (TP‑Link TL‑WDR4300 v1):

    scp bin/targets/ath79/generic/light_control_*.ipk root@<router_ip>:/tmp/
    ssh root@<router_ip>
    opkg install /tmp/light_control_*.ipk

## Start / stop

    /etc/init.d/light_control start
    /etc/init.d/light_control stop
    /etc/init.d/light_control restart

## Verifying operation

1. Confirm the service is running:

        ps | grep light_control

2. Check UBus registration:
    
        ubus list | grep light_control

3. Call the stats method:

        ubus call light_control get_stats

    Expected response:

        {
        "packets_received": 0,
        "bytes_received": 0,
        "errors": 0
        }

4. Check logs:
    
        logread | grep light_control | tail -n 30

5. Quickly clear logs before testing:

        logctl clear

6. Verify Tuya control:
   - Check that the dimmer responds to brightness changes:
     ```sh
     ubus call light_control set_brightness '{"value":50}'
     ```
   - Confirm in logs that the device accepted the command:
     ```sh
     logread | grep -E "Tuya: SET command succeeded|Tuya: recv()" | tail -n 20
     ```
   - Look for heartbeat activity (if enabled):
     ```sh
     logread | grep "Tuya: sending heartbeat" | tail -n 10
     ```
## Shutdown mechanism (current implementation)

Shutdown works deterministically due to the explicit loop in UbusExporter::run:

    void UbusExporter::run(std::atomic<bool>& stop_flag) {
        logger_.debug("UBus run(): starting event loop");

        uloop_init();

        if (ctx_) {
            ubus_add_uloop(ctx_);
        } else {
            logger_.warn("UBus context is not ready yet; waiting for reconnect timer");
        }

        // Key point: a timeout loop that regularly returns control
        while (!stop_flag.load(std::memory_order_acquire)) {
            uloop_run_timeout(200);
        }
        uloop_done();
        logger_.debug("UBus event loop stopped");
    }

What this achieves:

- uloop_run_timeout(200) does not block the thread for long: at most 200 ms between flag checks.
- When stop_flag = true is set, the loop exits quickly (within ~200 ms).
- uloop_done() properly releases uloop resources.
- As a result, the service stops within the procd timeout (5 seconds) and avoids SIGKILL.

Verify no SIGKILL is sent:

    logread | grep -E "SIGKILL|not stopped"

## Important notes (aligned with your development style)

- Do not remove the install target from CMakeLists.txt — it’s required for correct integration with the OpenWrt package.
- Keep the list of source files in one place (in CMakeLists.txt) and avoid duplication.
- Avoid excessive #if defined in main.cpp — move platform‑specific logic to separate modules.
- Do not copy project sources into the SDK — use an external path and feeds (or a direct link in feeds.conf).

>Security note: The repository must never contain real local_key values or any device-specific secrets. All keys are injected via UCI/runtime configuration and must not be hardcoded in source code or committed to Git.
The Tuya v3.3 implementation assumes that the local_key is provided in raw string form (not hex/MD5) and converted to binary internally.

## Next steps

- Add new UBus methods (brightness control, scenes).
- Integrate with ESP32 (UDP communication, state synchronization).
- Build a LuCI visualization interface.
- Add a local neural network (ready‑made open‑source models) for statistics analysis and adaptive control.    
